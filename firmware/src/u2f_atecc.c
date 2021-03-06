/*
 * Copyright (c) 2016, Conor Patrick
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

 *
 *
 * u2f_atecc.c
 * 		platform specific functionality for implementing U2F
 *
 */

#include "app.h"

#undef U2F_DISABLE
#ifndef U2F_DISABLE
#include "bsp.h"
#include "u2f.h"
#include "u2f_hid.h"
#include "eeprom.h"
#include "atecc508a.h"


static void gen_u2f_zero_tag(uint8_t * dst, uint8_t * appid, uint8_t * handle);

static struct u2f_hid_msg res;
static uint8_t* resbuf = (uint8_t*)&res;
static uint8_t resseq = 0;
static uint8_t serious = 0;


void u2f_response_writeback(uint8_t * buf, uint16_t len)
{
	u2f_hid_writeback(buf, len);
}

void u2f_response_flush()
{
	watchdog();
	u2f_hid_flush();
}

void u2f_response_start()
{
	watchdog();
}

int8_t u2f_get_user_feedback()
{
	uint32_t t;

	BUTTON_RESET_ON();                                // Clear ghost touches
	u2f_delay(6);
	BUTTON_RESET_OFF();
	t = get_ms();
	while (IS_BUTTON_PRESSED()) {                     // Wait to release button
		if (get_ms() - t > U2F_MS_USER_INPUT_WAIT) {  // 3 secs timeout
			return 1;
		}
		watchdog();
	}
	LedBlink(LED_BLINK_NUM_INF, 375);
	while(!IsButtonPressed())                         // Wait to push button
	{
		TaskLedBlink();                               // Run button driver
        TaskButton();                                 // Run led driver to ensure blinking
		if (get_ms() - t > U2F_MS_USER_INPUT_WAIT)    // 3 secs elapsed without button press
			break;                                    // Timeout
		watchdog();
	}

	if (IsButtonPressed()) {                          // Button has been pushed in time
		LedOn();
	} else {                                          // Button hasnt been pushed within the timeout
		LedBlink(LED_BLINK_NUM_INF, 375);
		return 1;                                     // Return error code
	}

	return 0;
}

#ifdef ATECC_SETUP_DEVICE
#define STATIC_IN_SETUP static
#else
#define STATIC_IN_SETUP
#endif


static uint8_t shabuf[70];
static uint8_t shaoffset = 0;
STATIC_IN_SETUP uint8_t SHA_FLAGS = 0;
STATIC_IN_SETUP uint8_t SHA_HMAC_KEY = 0;
static struct atecc_response res_digest;

STATIC_IN_SETUP void u2f_sha256_start()
{
	shaoffset = 0;
	atecc_send_recv(ATECC_CMD_SHA,
			SHA_FLAGS, SHA_HMAC_KEY,NULL,0,
			shabuf, sizeof(shabuf), NULL);
	SHA_HMAC_KEY = 0;
}


STATIC_IN_SETUP void u2f_sha256_update(uint8_t * buf, uint8_t len)
{
	uint8_t i = 0;
	watchdog();
	while(len--)
	{
		shabuf[shaoffset++] = *buf++;
		if (shaoffset == 64)
		{
			atecc_send_recv(ATECC_CMD_SHA,
					ATECC_SHA_UPDATE, 64,shabuf,64,
					shabuf, sizeof(shabuf), NULL);
			shaoffset = 0;
		}
	}
}


STATIC_IN_SETUP void u2f_sha256_finish()
{
	if (SHA_FLAGS == ATECC_SHA_START) SHA_FLAGS = ATECC_SHA_END;
	atecc_send_recv(ATECC_CMD_SHA,
			SHA_FLAGS, shaoffset,shabuf,shaoffset,
			shabuf, sizeof(shabuf), &res_digest);
	SHA_FLAGS = ATECC_SHA_START;
}

static int atecc_prep_encryption()
{
	struct atecc_response res;
	memset(appdata.tmp,0,32);
	if( atecc_send_recv(ATECC_CMD_NONCE,ATECC_NONCE_TEMP_UPDATE,0,
								appdata.tmp, 32,
								appdata.tmp, 40, &res) != 0 )
	{
		u2f_prints("pass through to tempkey failed\r\n");
		return -1;
	}
	if( atecc_send_recv(ATECC_CMD_GENDIG,
			ATECC_RW_DATA, U2F_MASTER_KEY_SLOT, NULL, 0,
			appdata.tmp, 40, &res) != 0)
	{
		u2f_prints("GENDIG failed\r\n");
		return -1;
	}

	return 0;
}

static void compute_key_hash(uint8_t * key, uint8_t * mask)
{
	// key must start with 4 zeros
	memset(appdata.tmp,0,28);
	memmove(appdata.tmp + 28, key, 36);

	u2f_sha256_start();

	u2f_sha256_update(mask,32);


	appdata.tmp[0] = ATECC_CMD_PRIVWRITE;
	appdata.tmp[1] = ATECC_PRIVWRITE_ENC;
	appdata.tmp[2] = 2;
	appdata.tmp[3] = 0;
	appdata.tmp[4] = 0xee;
	appdata.tmp[5] = 0x01;
	appdata.tmp[6] = 0x23;

	u2f_sha256_update(appdata.tmp,28 + 36);
	u2f_sha256_finish();
}

static int atecc_privwrite(int keyslot, uint8_t * key, uint8_t * mask, uint8_t * digest)
{
	struct atecc_response res;
	uint8_t i;

	atecc_prep_encryption();

	for (i=0; i<36; i++)
	{
		appdata.tmp[i] = key[i] ^ mask[i];
	}
	memmove(appdata.tmp+36, digest, 32);

	if( atecc_send_recv(ATECC_CMD_PRIVWRITE,
			ATECC_PRIVWRITE_ENC, keyslot, appdata.tmp, 68,
			appdata.tmp, 40, &res) != 0)
	{
		u2f_prints("PRIVWRITE failed\r\n");
		return -1;
	}
	return 0;
}


int8_t u2f_ecdsa_sign(uint8_t * dest, uint8_t * handle, uint8_t * appid)
{
	struct atecc_response res;
	uint16_t slot = U2F_TEMP_KEY_SLOT;
	if (handle == U2F_ATTESTATION_HANDLE)
	{
		slot = U2F_ATTESTATION_KEY_SLOT;
	}

	if( atecc_send_recv(ATECC_CMD_SIGN,
			ATECC_SIGN_EXTERNAL, slot, NULL, 0,
			appdata.tmp, 70, &res) != 0)
	{
		return -1;
	}
	memmove(dest, res.buf, 64);
	return 0;
}


// bad if this gets interrupted
int8_t u2f_new_keypair(uint8_t * handle, uint8_t * appid, uint8_t * pubkey)
{
	struct atecc_response res;
	uint8_t private_key[36];
	int i;

	watchdog();

	if (atecc_send_recv(ATECC_CMD_RNG,ATECC_RNG_P1,ATECC_RNG_P2,
		NULL, 0,
		appdata.tmp,
		sizeof(appdata.tmp), &res) != 0 )
	{
		return -1;
	}

	SHA_HMAC_KEY = U2F_MASTER_KEY_SLOT;
	SHA_FLAGS = ATECC_SHA_HMACSTART;
	u2f_sha256_start();
	u2f_sha256_update(appid,32);
	u2f_sha256_update(res.buf,4);
	SHA_FLAGS = ATECC_SHA_HMACEND;
	u2f_sha256_finish();

	memmove(handle, res.buf, 4);  // size of key handle must be 36

	memset(private_key,0,4);
	memmove(private_key+4, res_digest.buf, 32);

	for (i=4; i<36; i++)
	{
		private_key[i] ^= RMASK[i];
	}
	watchdog();
	compute_key_hash(private_key,  WMASK);
	memmove(handle+4, res_digest.buf, 32);  // size of key handle must be 36+8


	if ( atecc_privwrite(U2F_TEMP_KEY_SLOT, private_key, WMASK, handle+4) != 0)
	{
		return -1;
	}

	memset(private_key,0,36);

	if ( atecc_send_recv(ATECC_CMD_GENKEY,
			ATECC_GENKEY_PUBLIC, U2F_TEMP_KEY_SLOT, NULL, 0,
			appdata.tmp, 70, &res) != 0)
	{
		return -1;
	}

	memmove(pubkey, res.buf, 64);

	// the + 8
	gen_u2f_zero_tag(handle + U2F_KEY_HANDLE_KEY_SIZE, appid, handle);

	return 0;
}

int8_t u2f_load_key(uint8_t * handle, uint8_t * appid)
{
	uint8_t private_key[36];
	int i;

	watchdog();
	SHA_HMAC_KEY = U2F_MASTER_KEY_SLOT;
	SHA_FLAGS = ATECC_SHA_HMACSTART;
	u2f_sha256_start();
	u2f_sha256_update(appid,32);
	u2f_sha256_update(handle,4);
	SHA_FLAGS = ATECC_SHA_HMACEND;
	u2f_sha256_finish();

	memset(private_key,0,4);
	memmove(private_key+4, res_digest.buf, 32);
	for (i=4; i<36; i++)
	{
		private_key[i] ^= RMASK[i];
	}
	return atecc_privwrite(U2F_TEMP_KEY_SLOT, private_key, WMASK, handle+4);
}

static void gen_u2f_zero_tag(uint8_t * dst, uint8_t * appid, uint8_t * handle)
{
	const char * u2f_zero_const = "\xc1\xff\x67\x0d\x66\xe5\x55\xbb\xdc\x56\xaf\x7b\x41\x27\x4a\x21";
	SHA_HMAC_KEY = U2F_MASTER_KEY_SLOT;
	SHA_FLAGS = ATECC_SHA_HMACSTART;
	u2f_sha256_start();

	u2f_sha256_update(handle,U2F_KEY_HANDLE_KEY_SIZE);
	u2f_sha256_update(u2f_zero_const,16);
	u2f_sha256_update(appid,32);

	SHA_FLAGS = ATECC_SHA_HMACEND;
	u2f_sha256_finish();

	if (dst) memmove(dst, res_digest.buf, U2F_KEY_HANDLE_ID_SIZE);
}

int8_t u2f_appid_eq(uint8_t * handle, uint8_t * appid)
{
	gen_u2f_zero_tag(NULL,appid, handle);
	return memcmp(handle+U2F_KEY_HANDLE_KEY_SIZE, res_digest.buf, U2F_KEY_HANDLE_ID_SIZE);
}

uint32_t u2f_count()
{
	struct atecc_response res;
	atecc_send_recv(ATECC_CMD_COUNTER,
			ATECC_COUNTER_INC, ATECC_COUNTER0,NULL,0,
			appdata.tmp, sizeof(appdata.tmp), &res);
	return le32toh(*(uint32_t*)res.buf);
}

extern uint16_t __attest_size;
extern code char __attest[];

uint8_t * u2f_get_attestation_cert()
{
	return __attest;
}

uint16_t u2f_attestation_cert_size()
{
	return __attest_size;
}

void set_response_length(uint16_t len)
{
	u2f_hid_set_len(len);
}

#endif
