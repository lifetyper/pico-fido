/*
 * This file is part of the Pico FIDO distribution (https://github.com/polhenarejos/pico-fido).
 * Copyright (c) 2022 Pol Henarejos.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "common.h"
#include "ctap2_cbor.h"
#include "fido.h"
#include "ctap.h"
#include "bsp/board.h"
#include "files.h"
#include "apdu.h"
#include "credential.h"
#include "hsm.h"
#include "random.h"
#include "mbedtls/ecdh.h"
#include "mbedtls/chachapoly.h"
#include "mbedtls/hkdf.h"

extern uint8_t keydev_dec[32];
extern bool has_keydev_dec;

int cbor_config(const uint8_t *data, size_t len) {
    CborParser parser;
    CborValue map;
    CborError error = CborNoError;
    uint64_t subcommand = 0, pinUvAuthProtocol = 0, vendorCommandId = 0;
    CborByteString pinUvAuthParam = {0}, vendorAutCt = {0};
    size_t resp_size = 0;
    CborEncoder encoder, mapEncoder;

    CBOR_CHECK(cbor_parser_init(data, len, 0, &parser, &map));
    uint64_t val_c = 1;
    CBOR_PARSE_MAP_START(map, 1) {
        uint64_t val_u = 0;
        CBOR_FIELD_GET_UINT(val_u, 1);
        if (val_c <= 1 && val_c != val_u)
            CBOR_ERROR(CTAP2_ERR_MISSING_PARAMETER);
        if (val_u < val_c)
            CBOR_ERROR(CTAP2_ERR_INVALID_CBOR);
        val_c = val_u + 1;
        if (val_u == 0x01) {
            CBOR_FIELD_GET_UINT(subcommand, 1);
        }
        else if (val_u == 0x02) {
            uint64_t subpara = 0;
            CBOR_PARSE_MAP_START(_f1, 2) {
                if (subcommand == 0xff) {
                    CBOR_FIELD_GET_UINT(subpara, 2);
                    if (subpara == 0x01) {
                        CBOR_FIELD_GET_UINT(vendorCommandId, 2);
                    }
                    else if (subpara == 0x02) {
                        CBOR_FIELD_GET_BYTES(vendorAutCt, 2);
                    }
                }
            }
            CBOR_PARSE_MAP_END(_f1, 2);
        }
        else if (val_u == 0x03) {
            CBOR_FIELD_GET_UINT(pinUvAuthProtocol, 1);
        }
        else if (val_u == 0x04) { // pubKeyCredParams
            CBOR_FIELD_GET_BYTES(pinUvAuthParam, 1);
        }
    }
    CBOR_PARSE_MAP_END(map, 1);

    cbor_encoder_init(&encoder, ctap_resp->init.data + 1, CTAP_MAX_PACKET_SIZE, 0);

    if (subcommand == 0xff) {
        if (vendorCommandId == CTAP_CONFIG_AUT_DISABLE) {
            if (!file_has_data(ef_keydev_enc))
                CBOR_ERROR(CTAP2_ERR_NOT_ALLOWED);
            if (has_keydev_dec == false)
                CBOR_ERROR(CTAP2_ERR_PIN_AUTH_INVALID);
            flash_write_data_to_file(ef_keydev, keydev_dec, sizeof(keydev_dec));
            mbedtls_platform_zeroize(keydev_dec, sizeof(keydev_dec));
            flash_write_data_to_file(ef_keydev_enc, NULL, 0); // Set ef to 0 bytes
            low_flash_available();
        }
        else if (vendorCommandId == CTAP_CONFIG_AUT_ENABLE) {
            if (!file_has_data(ef_keydev))
                CBOR_ERROR(CTAP2_ERR_NOT_ALLOWED);
            if (mse.init == false)
                CBOR_ERROR(CTAP2_ERR_NOT_ALLOWED);

            mbedtls_chachapoly_context chatx;
            int ret = mse_decrypt_ct(vendorAutCt.data, vendorAutCt.len);
            if (ret != 0) {
                CBOR_ERROR(CTAP1_ERR_INVALID_PARAMETER);
            }

            uint8_t key_dev_enc[12+32+16];
            random_gen(NULL, key_dev_enc, 12);
            mbedtls_chachapoly_init(&chatx);
            mbedtls_chachapoly_setkey(&chatx, vendorAutCt.data);
            ret = mbedtls_chachapoly_encrypt_and_tag(&chatx, file_get_size(ef_keydev), key_dev_enc, NULL, 0, file_get_data(ef_keydev), key_dev_enc + 12, key_dev_enc + 12 + file_get_size(ef_keydev));
            mbedtls_chachapoly_free(&chatx);
            if (ret != 0){
                CBOR_ERROR(CTAP1_ERR_INVALID_PARAMETER);
            }

            flash_write_data_to_file(ef_keydev_enc, key_dev_enc, sizeof(key_dev_enc));
            mbedtls_platform_zeroize(key_dev_enc, sizeof(key_dev_enc));
            flash_write_data_to_file(ef_keydev, key_dev_enc, file_get_size(ef_keydev)); // Overwrite ef with 0
            flash_write_data_to_file(ef_keydev, NULL, 0); // Set ef to 0 bytes
            low_flash_available();
        }
        else {
            CBOR_ERROR(CTAP2_ERR_INVALID_SUBCOMMAND);
        }
        goto err;
    }
    else
        CBOR_ERROR(CTAP2_ERR_UNSUPPORTED_OPTION);
    CBOR_CHECK(cbor_encoder_close_container(&encoder, &mapEncoder));
    resp_size = cbor_encoder_get_buffer_size(&encoder, ctap_resp->init.data + 1);

    err:
    CBOR_FREE_BYTE_STRING(pinUvAuthParam);
    CBOR_FREE_BYTE_STRING(vendorAutCt);

    if (error != CborNoError) {
        if (error == CborErrorImproperValue)
            return CTAP2_ERR_CBOR_UNEXPECTED_TYPE;
        return error;
    }
    res_APDU_size = resp_size;
    return 0;
}
