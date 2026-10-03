#include "onenet_token.h"
#include "mbedtls/md5.h"
#include "mbedtls/md.h"
#include <string.h>
#include <stdio.h>


enum {
    BAD         = 0xFF,  /* invalid encoding */
    PAD         = '=',
    PEM_LINE_SZ = 64
};


static
const byte base64Encode[] = { 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J',
                              'K', 'L', 'M', 'N', 'O', 'P', 'Q', 'R', 'S', 'T',
                              'U', 'V', 'W', 'X', 'Y', 'Z',
                              'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j',
                              'k', 'l', 'm', 'n', 'o', 'p', 'q', 'r', 's', 't',
                              'u', 'v', 'w', 'x', 'y', 'z',
                              '0', '1', '2', '3', '4', '5', '6', '7', '8', '9',
                              '+', '/'
                            };

/* make sure *i (idx) won't exceed max, store and possibly escape to out,
 * raw means use e w/o decode,  0 on success */
static int CEscape(int escaped, byte e, byte* out, word32* i, word32 max,
                  int raw, int getSzOnly)
{
    int    doEscape = 0;
    word32 needed = 1;
    word32 idx = *i;

    byte basic;
    byte plus    = 0;
    byte equals  = 0;
    byte newline = 0;

    if (raw)
        basic = e;
    else
        basic = base64Encode[e];

    /* check whether to escape. Only escape for EncodeEsc */
    if (escaped == WC_ESC_NL_ENC) {
        switch ((char)basic) {
            case '+' :
                plus     = 1;
                doEscape = 1;
                needed  += 2;
                break;
            case '=' :
                equals   = 1;
                doEscape = 1;
                needed  += 2;
                break;
            case '\n' :
                newline  = 1;
                doEscape = 1;
                needed  += 2;
                break;
            default:
                /* do nothing */
                break;
        }
    }

    /* check size */
    if ( (idx+needed) > max && !getSzOnly) {
        return -132;
    }

    /* store it */
    if (doEscape == 0) {
        if(getSzOnly)
            idx++;
        else
            out[idx++] = basic;
    }
    else {
        if(getSzOnly)
            idx+=3;
        else {
            out[idx++] = '%';  /* start escape */

            if (plus) {
                out[idx++] = '2';
                out[idx++] = 'B';
            }
            else if (equals) {
                out[idx++] = '3';
                out[idx++] = 'D';
            }
            else if (newline) {
                out[idx++] = '0';
                out[idx++] = 'A';
            }
        }
    }
    *i = idx;

    return 0;
}

/* internal worker, handles both escaped and normal line endings.
   If out buffer is NULL, will return sz needed in outLen */
static int DoBase64_Encode(const byte* in, word32 inLen, byte* out,
                           word32* outLen, int escaped)
{
    int    ret = 0;
    word32 i = 0,
           j = 0,
           n = 0;   /* new line counter */

    int    getSzOnly = (out == NULL);

    word32 outSz = (inLen + 3 - 1) / 3 * 4;
    word32 addSz = (outSz + PEM_LINE_SZ - 1) / PEM_LINE_SZ;  /* new lines */

    if (escaped == WC_ESC_NL_ENC)
        addSz *= 3;   /* instead of just \n, we're doing %0A triplet */
    else if (escaped == WC_NO_NL_ENC)
        addSz = 0;    /* encode without \n */

    outSz += addSz;

    if (!outLen || (outSz > *outLen && !getSzOnly)) return -2;

    while (inLen > 2) {
        byte b1 = in[j++];
        byte b2 = in[j++];
        byte b3 = in[j++];


        byte e1 = b1 >> 2;
        byte e2 = (byte)(((b1 & 0x3) << 4) | (b2 >> 4));
        byte e3 = (byte)(((b2 & 0xF) << 2) | (b3 >> 6));
        byte e4 = b3 & 0x3F;


        ret = CEscape(escaped, e1, out, &i, *outLen, 0, getSzOnly);
        if (ret != 0) break;
        ret = CEscape(escaped, e2, out, &i, *outLen, 0, getSzOnly);
        if (ret != 0) break;
        ret = CEscape(escaped, e3, out, &i, *outLen, 0, getSzOnly);
        if (ret != 0) break;
        ret = CEscape(escaped, e4, out, &i, *outLen, 0, getSzOnly);
        if (ret != 0) break;

        inLen -= 3;


        if (escaped != WC_NO_NL_ENC && (++n % (PEM_LINE_SZ/4)) == 0 && inLen){
            ret = CEscape(escaped, '\n', out, &i, *outLen, 1, getSzOnly);
            if (ret != 0) break;
        }
    }

    if (inLen && ret == 0) {
        int twoBytes = (inLen == 2);

        byte b1 = in[j++];
        byte b2 = (twoBytes) ? in[j++] : 0;

        byte e1 = b1 >> 2;
        byte e2 = (byte)(((b1 & 0x3) << 4) | (b2 >> 4));
        byte e3 = (byte)((b2 & 0xF) << 2);

        ret = CEscape(escaped, e1, out, &i, *outLen, 0, getSzOnly);
        if (ret == 0)
            ret = CEscape(escaped, e2, out, &i, *outLen, 0, getSzOnly);
        if (ret == 0) {
  
            if (twoBytes)
                ret = CEscape(escaped, e3, out, &i, *outLen, 0, getSzOnly);
            else
                ret = CEscape(escaped, '=', out, &i, *outLen, 1, getSzOnly);
        }

        if (ret == 0)
            ret = CEscape(escaped, '=', out, &i, *outLen, 1, getSzOnly);
    }

    if (ret == 0 && escaped != WC_NO_NL_ENC)
        ret = CEscape(escaped, '\n', out, &i, *outLen, 1, getSzOnly);

    if (i != outSz && escaped != 1 && ret == 0)
        return -154;

    *outLen = i;
    if(ret == 0)
        return getSzOnly ? -202 : 0;
    return ret;
}

int Base64_Encode(const byte* in, word32 inLen, byte* out, word32* outLen)
{
    return DoBase64_Encode(in, inLen, out, outLen, WC_STD_ENC);
}


int Base64_EncodeEsc(const byte* in, word32 inLen, byte* out, word32* outLen)
{
    return DoBase64_Encode(in, inLen, out, outLen, WC_ESC_NL_ENC);
}

int Base64_Encode_NoNl(const byte* in, word32 inLen, byte* out, word32* outLen)
{
    return DoBase64_Encode(in, inLen, out, outLen, WC_NO_NL_ENC);
}


static
const byte base64Decode[] = { 62, BAD, BAD, BAD, 63,   /* + starts at 0x2B */
                              52, 53, 54, 55, 56, 57, 58, 59, 60, 61,
                              BAD, BAD, BAD, BAD, BAD, BAD, BAD,
                              0, 1, 2, 3, 4, 5, 6, 7, 8, 9,
                              10, 11, 12, 13, 14, 15, 16, 17, 18, 19,
                              20, 21, 22, 23, 24, 25,
                              BAD, BAD, BAD, BAD, BAD, BAD,
                              26, 27, 28, 29, 30, 31, 32, 33, 34, 35,
                              36, 37, 38, 39, 40, 41, 42, 43, 44, 45,
                              46, 47, 48, 49, 50, 51
                            };


int Base64_Decode(const byte* in, word32 inLen, byte* out, word32* outLen)
{
    word32 i = 0;
    word32 j = 0;
    word32 plainSz = inLen - ((inLen + (PEM_LINE_SZ - 1)) / PEM_LINE_SZ );
    const byte maxIdx = (byte)sizeof(base64Decode) + 0x2B - 1;

    plainSz = (plainSz * 3 + 3) / 4;
    if (plainSz > *outLen) return -173;

    while (inLen > 3) {
        byte b1, b2, b3;
        byte e1 = in[j++];
        byte e2 = in[j++];
        byte e3 = in[j++];
        byte e4 = in[j++];

        int pad3 = 0;
        int pad4 = 0;

        if (e1 == 0)            /* end file 0's */
            break;
        if (e3 == PAD)
            pad3 = 1;
        if (e4 == PAD)
            pad4 = 1;

        if (e1 < 0x2B || e2 < 0x2B || e3 < 0x2B || e4 < 0x2B) {
            return -154;
        }

        if (e1 > maxIdx || e2 > maxIdx || e3 > maxIdx || e4 > maxIdx) {
            return -154;
        }

        e1 = base64Decode[e1 - 0x2B];
        e2 = base64Decode[e2 - 0x2B];
        e3 = (e3 == PAD) ? 0 : base64Decode[e3 - 0x2B];
        e4 = (e4 == PAD) ? 0 : base64Decode[e4 - 0x2B];

        b1 = (byte)((e1 << 2) | (e2 >> 4));
        b2 = (byte)(((e2 & 0xF) << 4) | (e3 >> 2));
        b3 = (byte)(((e3 & 0x3) << 6) | e4);

        out[i++] = b1;
        if (!pad3)
            out[i++] = b2;
        if (!pad4)
            out[i++] = b3;
        else
            break;

        inLen -= 4;
        if (inLen && (in[j] == ' ' || in[j] == '\r' || in[j] == '\n')) {
            byte endLine = in[j++];
            inLen--;
            while (inLen && endLine == ' ') {   
                endLine = in[j++];
                inLen--;
            }
            if (endLine == '\r') {
                if (inLen) {
                    endLine = in[j++];
                    inLen--;
                }
            }
            if (endLine != '\n') {
                return -154;
            }
        }
    }
    *outLen = i;

    return 0;
}

/**
 * 计算hmd
 * @param key 秘钥
 * @param content 内容
 * @param output 输出md5值
 * @return 无
 */
static void calc_hmd(enum sig_method_e method,unsigned char* key,size_t key_len,unsigned char *content,size_t content_len,unsigned char *output)
{
    mbedtls_md_context_t md_ctx;
    const mbedtls_md_info_t *md_info = NULL;
    if (SIG_METHOD_MD5 == method) {
        md_info = mbedtls_md_info_from_type(MBEDTLS_MD_MD5);
    } else if (SIG_METHOD_SHA1 == method) {
        md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA1);
    } else if (SIG_METHOD_SHA256 == method) {
        md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    }

    mbedtls_md_init(&md_ctx);
    mbedtls_md_setup(&md_ctx, md_info, 1);
    mbedtls_md_hmac_starts(&md_ctx,key,key_len);
    mbedtls_md_hmac_update(&md_ctx,content,content_len);
    mbedtls_md_hmac_finish(&md_ctx,output);
    mbedtls_md_free(&md_ctx);
}


#define DEV_TOKEN_LEN 256
#define DEV_TOKEN_VERISON_STR "2018-10-31"

#define DEV_TOKEN_SIG_METHOD_MD5 "md5"
#define DEV_TOKEN_SIG_METHOD_SHA1 "sha1"
#define DEV_TOKEN_SIG_METHOD_SHA256 "sha256"

//计算token
int32_t
dev_token_generate(char* token, enum sig_method_e method, uint32_t exp_time, const char* product_id, const char* dev_name, const char* access_key)
{
    if (!token || !product_id || !access_key) return -1;

    uint8_t  base64_data[64] = { 0 };
    uint8_t  str_for_sig[128] = { 0 };
    uint8_t  sign_buf[128]    = { 0 };
    unsigned int base64_data_len = sizeof(base64_data);
    const char* sig_method_str = NULL;
    unsigned int sign_len        = 0;

    if (SIG_METHOD_MD5 == method) {
        sig_method_str = DEV_TOKEN_SIG_METHOD_MD5;
        sign_len       = 16;
    } else if (SIG_METHOD_SHA1 == method) {
        sig_method_str = DEV_TOKEN_SIG_METHOD_SHA1;
        sign_len       = 20;
    } else if (SIG_METHOD_SHA256 == method) {
        sig_method_str = DEV_TOKEN_SIG_METHOD_SHA256;
        sign_len       = 32;
    } else {
        return -2;
    }

    int sig_text_len;
    if (dev_name) {
        sig_text_len = snprintf((char*)str_for_sig, sizeof(str_for_sig),
                                "%lu\n%s\nproducts/%s/devices/%s\n%s",
                                exp_time, sig_method_str, product_id, dev_name,
                                DEV_TOKEN_VERISON_STR);
    } else {
        sig_text_len = snprintf((char*)str_for_sig, sizeof(str_for_sig),
                                "%lu\n%s\nproducts/%s\n%s",
                                exp_time, sig_method_str, product_id,
                                DEV_TOKEN_VERISON_STR);
    }
    if (sig_text_len < 0 || sig_text_len >= (int)sizeof(str_for_sig)) return -3;

    if (Base64_Decode((const byte*)access_key, strlen(access_key),
                      base64_data, &base64_data_len) != 0) return -4;

    calc_hmd(method,base64_data,base64_data_len,str_for_sig,strlen((char*)str_for_sig),sign_buf);

    memset(base64_data, 0, sizeof(base64_data));
    base64_data_len = sizeof(base64_data);
    if (Base64_Encode_NoNl(sign_buf, sign_len, base64_data,
                           &base64_data_len) != 0) return -5;

    size_t used;
    int written;
    if (dev_name) {
        written = snprintf(token, DEV_TOKEN_LEN,
                           "version=%s&res=products%%2F%s%%2Fdevices%%2F%s&et=%lu&method=%s&sign=",
                           DEV_TOKEN_VERISON_STR, product_id, dev_name,
                           exp_time, sig_method_str);
    } else {
        written = snprintf(token, DEV_TOKEN_LEN,
                           "version=%s&res=products%%2F%s&et=%lu&method=%s&sign=",
                           DEV_TOKEN_VERISON_STR, product_id, exp_time,
                           sig_method_str);
    }
    if (written < 0 || written >= DEV_TOKEN_LEN) return -6;
    used = (size_t)written;

    for (uint32_t i = 0; i < base64_data_len; i++) {
        const char *escaped = NULL;
        switch (base64_data[i]) {
            case '+': escaped = "%2B"; break;
            case ' ': escaped = "%20"; break;
            case '/': escaped = "%2F"; break;
            case '?': escaped = "%3F"; break;
            case '%': escaped = "%25"; break;
            case '#': escaped = "%23"; break;
            case '&': escaped = "%26"; break;
            case '=': escaped = "%3D"; break;
            default: break;
        }

        size_t add = escaped ? 3 : 1;
        if (used + add >= DEV_TOKEN_LEN) return -7;
        if (escaped) memcpy(token + used, escaped, add);
        else token[used] = (char)base64_data[i];
        used += add;
        token[used] = '\0';
    }

    return 0;
}
