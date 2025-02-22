// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

package snowflake

import (
	"errors"
	"runtime/debug"
	"strings"

	"github.com/apache/arrow-adbc/go/adbc"
	"github.com/apache/arrow-adbc/go/adbc/driver/driverbase"
	"github.com/apache/arrow/go/v13/arrow/memory"
	"github.com/snowflakedb/gosnowflake"
	"golang.org/x/exp/maps"
)

const (
	infoDriverName = "ADBC Snowflake Driver - Go"
	infoVendorName = "Snowflake"

	OptionDatabase  = "adbc.snowflake.sql.db"
	OptionSchema    = "adbc.snowflake.sql.schema"
	OptionWarehouse = "adbc.snowflake.sql.warehouse"
	OptionRole      = "adbc.snowflake.sql.role"
	OptionRegion    = "adbc.snowflake.sql.region"
	OptionAccount   = "adbc.snowflake.sql.account"
	OptionProtocol  = "adbc.snowflake.sql.uri.protocol"
	OptionPort      = "adbc.snowflake.sql.uri.port"
	OptionHost      = "adbc.snowflake.sql.uri.host"
	// Specify auth type to use for snowflake connection based on
	// what is supported by the snowflake driver. Default is
	// "auth_snowflake" (use OptionValueAuth* consts to specify desired
	// authentication type).
	OptionAuthType = "adbc.snowflake.sql.auth_type"
	// Login retry timeout EXCLUDING network roundtrip and reading http response
	// use format like http://pkg.go.dev/time#ParseDuration such as
	// "300ms", "1.5s" or "1m30s". ParseDuration accepts negative values
	// but the absolute value will be used.
	OptionLoginTimeout = "adbc.snowflake.sql.client_option.login_timeout"
	// request retry timeout EXCLUDING network roundtrip and reading http response
	// use format like http://pkg.go.dev/time#ParseDuration such as
	// "300ms", "1.5s" or "1m30s". ParseDuration accepts negative values
	// but the absolute value will be used.
	OptionRequestTimeout = "adbc.snowflake.sql.client_option.request_timeout"
	// JWT expiration after timeout
	// use format like http://pkg.go.dev/time#ParseDuration such as
	// "300ms", "1.5s" or "1m30s". ParseDuration accepts negative values
	// but the absolute value will be used.
	OptionJwtExpireTimeout = "adbc.snowflake.sql.client_option.jwt_expire_timeout"
	// Timeout for network round trip + reading http response
	// use format like http://pkg.go.dev/time#ParseDuration such as
	// "300ms", "1.5s" or "1m30s". ParseDuration accepts negative values
	// but the absolute value will be used.
	OptionClientTimeout = "adbc.snowflake.sql.client_option.client_timeout"

	OptionApplicationName  = "adbc.snowflake.sql.client_option.app_name"
	OptionSSLSkipVerify    = "adbc.snowflake.sql.client_option.tls_skip_verify"
	OptionOCSPFailOpenMode = "adbc.snowflake.sql.client_option.ocsp_fail_open_mode"
	// specify the token to use for OAuth or other forms of authentication
	OptionAuthToken = "adbc.snowflake.sql.client_option.auth_token"
	// specify the OKTAUrl to use for OKTA Authentication
	OptionAuthOktaUrl = "adbc.snowflake.sql.client_option.okta_url"
	// enable the session to persist even after the connection is closed
	OptionKeepSessionAlive = "adbc.snowflake.sql.client_option.keep_session_alive"
	// specify the RSA private key to use to sign the JWT
	// this should point to a file containing a PKCS1 private key to be
	// loaded. Commonly encoded in PEM blocks of type "RSA PRIVATE KEY"
	OptionJwtPrivateKey    = "adbc.snowflake.sql.client_option.jwt_private_key"
	OptionDisableTelemetry = "adbc.snowflake.sql.client_option.disable_telemetry"
	// snowflake driver logging level
	OptionLogTracing = "adbc.snowflake.sql.client_option.tracing"
	// When true, the MFA token is cached in the credential manager. True by default
	// on Windows/OSX, false for Linux
	OptionClientRequestMFAToken = "adbc.snowflake.sql.client_option.cache_mfa_token"
	// When true, the ID token is cached in the credential manager. True by default
	// on Windows/OSX, false for Linux
	OptionClientStoreTempCred = "adbc.snowflake.sql.client_option.store_temp_creds"

	// auth types are implemented by the Snowflake driver in gosnowflake
	// general username password authentication
	OptionValueAuthSnowflake = "auth_snowflake"
	// use OAuth authentication for snowflake connection
	OptionValueAuthOAuth = "auth_oauth"
	// use an external browser to access a FED and perform SSO auth
	OptionValueAuthExternalBrowser = "auth_ext_browser"
	// use a native OKTA URL to perform SSO authentication on Okta
	OptionValueAuthOkta = "auth_okta"
	// use a JWT to perform authentication
	OptionValueAuthJwt = "auth_jwt"
	// use a username and password with mfa
	OptionValueAuthUserPassMFA = "auth_mfa"
)

var (
	infoDriverVersion      string
	infoDriverArrowVersion string
	infoSupportedCodes     []adbc.InfoCode
)

func init() {
	if info, ok := debug.ReadBuildInfo(); ok {
		for _, dep := range info.Deps {
			switch {
			case dep.Path == "github.com/apache/arrow-adbc/go/adbc/driver/snowflake":
				infoDriverVersion = dep.Version
			case strings.HasPrefix(dep.Path, "github.com/apache/arrow/go/"):
				infoDriverArrowVersion = dep.Version
			}
		}
	}
	// XXX: Deps not populated in tests
	// https://github.com/golang/go/issues/33976
	if infoDriverVersion == "" {
		infoDriverVersion = "(unknown or development build)"
	}
	if infoDriverArrowVersion == "" {
		infoDriverArrowVersion = "(unknown or development build)"
	}

	infoSupportedCodes = []adbc.InfoCode{
		adbc.InfoDriverName,
		adbc.InfoDriverVersion,
		adbc.InfoDriverArrowVersion,
		adbc.InfoVendorName,
	}
}

func errToAdbcErr(code adbc.Status, err error) error {
	if err == nil {
		return nil
	}

	var e adbc.Error
	if errors.As(err, &e) {
		e.Code = code
		return e
	}

	var sferr *gosnowflake.SnowflakeError
	if errors.As(err, &sferr) {
		var sqlstate [5]byte
		copy(sqlstate[:], []byte(sferr.SQLState))

		if sferr.SQLState == "42S02" {
			code = adbc.StatusNotFound
		}

		return adbc.Error{
			Code:       code,
			Msg:        sferr.Error(),
			VendorCode: int32(sferr.Number),
			SqlState:   sqlstate,
		}
	}

	return adbc.Error{
		Msg:  err.Error(),
		Code: code,
	}
}

type driverImpl struct {
	driverbase.DriverImplBase
}

// NewDriver creates a new Snowflake driver using the given Arrow allocator.
func NewDriver(alloc memory.Allocator) adbc.Driver {
	return driverbase.NewDriver(&driverImpl{DriverImplBase: driverbase.NewDriverImplBase("Snowflake", alloc)})
}

func (d *driverImpl) NewDatabase(opts map[string]string) (adbc.Database, error) {
	opts = maps.Clone(opts)
	db := &databaseImpl{DatabaseImplBase: driverbase.NewDatabaseImplBase(&d.DriverImplBase)}
	if err := db.SetOptions(opts); err != nil {
		return nil, err
	}
	return driverbase.NewDatabase(db), nil
}
