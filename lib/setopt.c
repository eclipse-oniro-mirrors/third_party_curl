/***************************************************************************
 *                                  _   _ ____  _
 *  Project                     ___| | | |  _ \| |
 *                             / __| | | | |_) | |
 *                            | (__| |_| |  _ <| |___
 *                             \___|\___/|_| \_\_____|
 *
 * Copyright (C) Daniel Stenberg, <daniel@haxx.se>, et al.
 *
 * This software is licensed as described in the file COPYING, which
 * you should have received as part of this distribution. The terms
 * are also available at https://curl.se/docs/copyright.html.
 *
 * You may opt to use, copy, modify, merge, publish, distribute and/or sell
 * copies of the Software, and permit persons to whom the Software is
 * furnished to do so, under the terms of the COPYING file.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 * SPDX-License-Identifier: curl
 *
 ***************************************************************************/

#include "curl_setup.h"

#include <limits.h>

#ifdef HAVE_NETINET_IN_H
#include <netinet/in.h>
#endif

#ifdef HAVE_LINUX_TCP_H
#include <linux/tcp.h>
#elif defined(HAVE_NETINET_TCP_H)
#include <netinet/tcp.h>
#endif

#include "urldata.h"
#include "url.h"
#include "progress.h"
#include "content_encoding.h"
#include "strcase.h"
#include "share.h"
#include "vtls/vtls.h"
#include "warnless.h"
#include "sendf.h"
#include "http2.h"
#include "setopt.h"
#include "multiif.h"
#include "altsvc.h"
#include "hsts.h"
#include "tftp.h"
#include "strdup.h"
#include "escape.h"

/* The last 3 #include files should be in this order */
#include "curl_printf.h"
#include "curl_memory.h"
#include "memdebug.h"

CURLcode Curl_setstropt(char **charp, const char *s)
{
  /* Release the previous storage at `charp' and replace by a dynamic storage
     copy of `s'. Return CURLE_OK or CURLE_OUT_OF_MEMORY. */

  Curl_safefree(*charp);

  if(s) {
    if(strlen(s) > CURL_MAX_INPUT_LENGTH)
      return CURLE_BAD_FUNCTION_ARGUMENT;

    *charp = strdup(s);
    if(!*charp)
      return CURLE_OUT_OF_MEMORY;
  }

  return CURLE_OK;
}

CURLcode Curl_setblobopt(struct curl_blob **blobp,
                         const struct curl_blob *blob)
{
  /* free the previous storage at `blobp' and replace by a dynamic storage
     copy of blob. If CURL_BLOB_COPY is set, the data is copied. */

  Curl_safefree(*blobp);

  if(blob) {
    struct curl_blob *nblob;
    if(blob->len > CURL_MAX_INPUT_LENGTH)
      return CURLE_BAD_FUNCTION_ARGUMENT;
    nblob = (struct curl_blob *)
      malloc(sizeof(struct curl_blob) +
             ((blob->flags & CURL_BLOB_COPY) ? blob->len : 0));
    if(!nblob)
      return CURLE_OUT_OF_MEMORY;
    *nblob = *blob;
    if(blob->flags & CURL_BLOB_COPY) {
      /* put the data after the blob struct in memory */
      nblob->data = (char *)nblob + sizeof(struct curl_blob);
      memcpy(nblob->data, blob->data, blob->len);
    }

    *blobp = nblob;
    return CURLE_OK;
  }

  return CURLE_OK;
}

static CURLcode setstropt_userpwd(char *option, char **userp, char **passwdp)
{
  char *user = NULL;
  char *passwd = NULL;

  DEBUGASSERT(userp);
  DEBUGASSERT(passwdp);

  /* Parse the login details if specified. It not then we treat NULL as a hint
     to clear the existing data */
  if(option) {
    size_t len = strlen(option);
    CURLcode result;
    if(len > CURL_MAX_INPUT_LENGTH)
      return CURLE_BAD_FUNCTION_ARGUMENT;

    result = Curl_parse_login_details(option, len, &user, &passwd, NULL);
    if(result)
      return result;
  }

  free(*userp);
  *userp = user;

  free(*passwdp);
  *passwdp = passwd;

  return CURLE_OK;
}

#define C_SSLVERSION_VALUE(x) (x & 0xffff)
#define C_SSLVERSION_MAX_VALUE(x) (x & 0xffff0000)

static CURLcode protocol2num(const char *str, curl_prot_t *val)
{
  /*
   * We are asked to cherry-pick protocols, so play it safe and disallow all
   * protocols to start with, and re-add the wanted ones back in.
   */
  *val = 0;

  if(!str)
    return CURLE_BAD_FUNCTION_ARGUMENT;

  if(curl_strequal(str, "all")) {
    *val = ~(curl_prot_t) 0;
    return CURLE_OK;
  }

  do {
    const char *token = str;
    size_t tlen;

    str = strchr(str, ',');
    tlen = str? (size_t) (str - token): strlen(token);
    if(tlen) {
      const struct Curl_handler *h = Curl_getn_scheme_handler(token, tlen);

      if(!h)
        return CURLE_UNSUPPORTED_PROTOCOL;

      *val |= h->protocol;
    }
  } while(str && str++);

  if(!*val)
    /* no protocol listed */
    return CURLE_BAD_FUNCTION_ARGUMENT;
  return CURLE_OK;
}

/*
 * Do not make Curl_vsetopt() static: it is called from
 * packages/OS400/ccsidcurl.c.
 */
CURLcode Curl_vsetopt(struct Curl_easy *data, CURLoption option, va_list param)
{
  char *argptr;
  CURLcode result = CURLE_OK;
  long arg;
  unsigned long uarg;
  curl_off_t bigsize;

  switch(option) {
  case CURLOPT_DNS_CACHE_TIMEOUT:
    arg = va_arg(param, long);
    if(arg < -1)
      return CURLE_BAD_FUNCTION_ARGUMENT;
    else if(arg > INT_MAX)
      arg = INT_MAX;

    data->set.dns_cache_timeout = (int)arg;
    break;
  case CURLOPT_CA_CACHE_TIMEOUT:
    arg = va_arg(param, long);
    if(arg < -1)
      return CURLE_BAD_FUNCTION_ARGUMENT;
    else if(arg > INT_MAX)
      arg = INT_MAX;

    data->set.general_ssl.ca_cache_timeout = (int)arg;
    break;
  case CURLOPT_DNS_USE_GLOBAL_CACHE:
    /* deprecated */
    break;
  case CURLOPT_SSL_CIPHER_LIST:
    /* set a list of cipher we want to use in the SSL connection */
    result = Curl_setstropt(&data->set.str[STRING_SSL_CIPHER_LIST],
                            va_arg(param, char *));
    break;
#ifndef CURL_DISABLE_PROXY
  case CURLOPT_PROXY_SSL_CIPHER_LIST:
    /* set a list of cipher we want to use in the SSL connection for proxy */
    result = Curl_setstropt(&data->set.str[STRING_SSL_CIPHER_LIST_PROXY],
                            va_arg(param, char *));
    break;
#endif
  case CURLOPT_TLS13_CIPHERS:
    if(Curl_ssl_supports(data, SSLSUPP_TLS13_CIPHERSUITES)) {
      /* set preferred list of TLS 1.3 cipher suites */
      result = Curl_setstropt(&data->set.str[STRING_SSL_CIPHER13_LIST],
                              va_arg(param, char *));
    }
    else
      return CURLE_NOT_BUILT_IN;
    break;
#ifndef CURL_DISABLE_PROXY
  case CURLOPT_PROXY_TLS13_CIPHERS:
    if(Curl_ssl_supports(data, SSLSUPP_TLS13_CIPHERSUITES)) {
      /* set preferred list of TLS 1.3 cipher suites for proxy */
      result = Curl_setstropt(&data->set.str[STRING_SSL_CIPHER13_LIST_PROXY],
                              va_arg(param, char *));
    }
    else
      return CURLE_NOT_BUILT_IN;
    break;
#endif
  case CURLOPT_RANDOM_FILE:
    break;
  case CURLOPT_EGDSOCKET:
    break;
  case CURLOPT_MAXCONNECTS:
    /*
     * Set the absolute number of maximum simultaneous alive connection that
     * libcurl is allowed to have.
     */
    uarg = va_arg(param, unsigned long);
    if(uarg > UINT_MAX)
      return CURLE_BAD_FUNCTION_ARGUMENT;
    data->set.maxconnects = (unsigned int)uarg;
    break;
  case CURLOPT_FORBID_REUSE:
    /*
     * When this transfer is done, it must not be left to be reused by a
     * subsequent transfer but shall be closed immediately.
     */
    data->set.reuse_forbid = (0 != va_arg(param, long));
    break;
  case CURLOPT_FRESH_CONNECT:
    /*
     * This transfer shall not use a previously cached connection but
     * should be made with a fresh new connect!
     */
    data->set.reuse_fresh = (0 != va_arg(param, long));
    break;
  case CURLOPT_VERBOSE:
    /*
     * Verbose means infof() calls that give a lot of information about
     * the connection and transfer procedures as well as internal choices.
     */
    data->set.verbose = (0 != va_arg(param, long));
    break;
  case CURLOPT_HEADER:
    /*
     * Set to include the header in the general data output stream.
     */
    data->set.include_header = (0 != va_arg(param, long));
    break;
  case CURLOPT_NOPROGRESS:
    /*
     * Shut off the internal supported progress meter
     */
    data->set.hide_progress = (0 != va_arg(param, long));
    if(data->set.hide_progress)
      data->progress.flags |= PGRS_HIDE;
    else
      data->progress.flags &= ~PGRS_HIDE;
    break;
  case CURLOPT_NOBODY:
    /*
     * Do not include the body part in the output data stream.
     */
    data->set.opt_no_body = (0 != va_arg(param, long));
#ifndef CURL_DISABLE_HTTP
    if(data->set.opt_no_body)
      /* in HTTP lingo, no body means using the HEAD request... */
      data->set.method = HTTPREQ_HEAD;
    else if(data->set.method == HTTPREQ_HEAD)
      data->set.method = HTTPREQ_GET;
#endif
    break;
  case CURLOPT_FAILONERROR:
    /*
     * Don't output the >=400 error code HTML-page, but instead only
     * return error.
     */
    data->set.http_fail_on_error = (0 != va_arg(param, long));
    break;
  case CURLOPT_KEEP_SENDING_ON_ERROR:
    data->set.http_keep_sending_on_error = (0 != va_arg(param, long));
    break;
  case CURLOPT_UPLOAD:
  case CURLOPT_PUT:
    /*
     * We want to sent data to the remote host. If this is HTTP, that equals
     * using the PUT request.
     */
    arg = va_arg(param, long);
    if(arg) {
      /* If this is HTTP, PUT is what's needed to "upload" */
      data->set.method = HTTPREQ_PUT;
      data->set.opt_no_body = FALSE; /* this is implied */
    }
    else
      /* In HTTP, the opposite of upload is GET (unless NOBODY is true as
         then this can be changed to HEAD later on) */
      data->set.method = HTTPREQ_GET;
    break;
  case CURLOPT_REQUEST_TARGET:
    result = Curl_setstropt(&data->set.str[STRING_TARGET],
                            va_arg(param, char *));
    break;
  case CURLOPT_FILETIME:
    /*
     * Try to get the file time of the remote document. The time will
     * later (possibly) become available using curl_easy_getinfo().
     */
    data->set.get_filetime = (0 != va_arg(param, long));
    break;
  case CURLOPT_SERVER_RESPONSE_TIMEOUT:
    /*
     * Option that specifies how quickly a server response must be obtained
     * before it is considered failure. For pingpong protocols.
     */
    arg = va_arg(param, long);
    if((arg >= 0) && (arg <= (INT_MAX/1000)))
      data->set.server_response_timeout = (unsigned int)arg * 1000;
    else
      return CURLE_BAD_FUNCTION_ARGUMENT;
    break;
  case CURLOPT_SERVER_RESPONSE_TIMEOUT_MS:
    /*
     * Option that specifies how quickly a server response must be obtained
     * before it is considered failure. For pingpong protocols.
     */
    arg = va_arg(param, long);
    if((arg >= 0) && (arg <= INT_MAX))
      data->set.server_response_timeout = (unsigned int)arg;
    else
      return CURLE_BAD_FUNCTION_ARGUMENT;
    break;
#ifndef CURL_DISABLE_TFTP
  case CURLOPT_TFTP_NO_OPTIONS:
    /*
     * Option that prevents libcurl from sending TFTP option requests to the
     * server.
     */
    data->set.tftp_no_options = va_arg(param, long) != 0;
    break;
  case CURLOPT_TFTP_BLKSIZE:
    /*
     * TFTP option that specifies the block size to use for data transmission.
     */
    arg = va_arg(param, long);
    if(arg > TFTP_BLKSIZE_MAX || arg < TFTP_BLKSIZE_MIN)
      return CURLE_BAD_FUNCTION_ARGUMENT;
    data->set.tftp_blksize = arg;
    break;
#endif
#ifndef CURL_DISABLE_NETRC
  case CURLOPT_NETRC:
    /*
     * Parse the $HOME/.netrc file
     */
    arg = va_arg(param, long);
    if((arg < CURL_NETRC_IGNORED) || (arg >= CURL_NETRC_LAST))
      return CURLE_BAD_FUNCTION_ARGUMENT;
    data->set.use_netrc = (unsigned char)arg;
    break;
  case CURLOPT_NETRC_FILE:
    /*
     * Use this file instead of the $HOME/.netrc file
     */
    result = Curl_setstropt(&data->set.str[STRING_NETRC_FILE],
                            va_arg(param, char *));
    break;
#endif
  case CURLOPT_TRANSFERTEXT:
    /*
     * This option was previously named 'FTPASCII'. Renamed to work with
     * more protocols than merely FTP.
     *
     * Transfer using ASCII (instead of BINARY).
     */
    data->set.prefer_ascii = (0 != va_arg(param, long));
    break;
  case CURLOPT_TIMECONDITION:
    /*
     * Set HTTP time condition. This must be one of the defines in the
     * curl/curl.h header file.
     */
    arg = va_arg(param, long);
    if((arg < CURL_TIMECOND_NONE) || (arg >= CURL_TIMECOND_LAST))
      return CURLE_BAD_FUNCTION_ARGUMENT;
    data->set.timecondition = (unsigned char)(curl_TimeCond)arg;
    break;
  case CURLOPT_TIMEVALUE:
    /*
     * This is the value to compare with the remote document with the
     * method set with CURLOPT_TIMECONDITION
     */
    data->set.timevalue = (time_t)va_arg(param, long);
    break;

  case CURLOPT_TIMEVALUE_LARGE:
    /*
     * This is the value to compare with the remote document with the
     * method set with CURLOPT_TIMECONDITION
     */
    data->set.timevalue = (time_t)va_arg(param, curl_off_t);
    break;

  case CURLOPT_SSLVERSION:
#ifndef CURL_DISABLE_PROXY
  case CURLOPT_PROXY_SSLVERSION:
#endif
    /*
     * Set explicit SSL version to try to connect with, as some SSL
     * implementations are lame.
     */
#ifdef USE_SSL
    {
      long version, version_max;
      struct ssl_primary_config *primary = &data->set.ssl.primary;
#ifndef CURL_DISABLE_PROXY
      if(option != CURLOPT_SSLVERSION)
        primary = &data->set.proxy_ssl.primary;
#endif

      arg = va_arg(param, long);

      version = C_SSLVERSION_VALUE(arg);
      version_max = C_SSLVERSION_MAX_VALUE(arg);

      if(version < CURL_SSLVERSION_DEFAULT ||
         version == CURL_SSLVERSION_SSLv2 ||
         version == CURL_SSLVERSION_SSLv3 ||
         version >= CURL_SSLVERSION_LAST ||
         version_max < CURL_SSLVERSION_MAX_NONE ||
         version_max >= CURL_SSLVERSION_MAX_LAST)
        return CURLE_BAD_FUNCTION_ARGUMENT;

      primary->version = (unsigned char)version;
      primary->version_max = (unsigned int)version_max;
    }
#else
    result = CURLE_NOT_BUILT_IN;
#endif
    break;

    /* MQTT "borrows" some of the HTTP options */
#if !defined(CURL_DISABLE_HTTP) || !defined(CURL_DISABLE_MQTT)
  case CURLOPT_COPYPOSTFIELDS:
    /*
     * A string with POST data. Makes curl HTTP POST. Even if it is NULL.
     * If needed, CURLOPT_POSTFIELDSIZE must have been set prior to
     *  CURLOPT_COPYPOSTFIELDS and not altered later.
     */
    argptr = va_arg(param, char *);

    if(!argptr || data->set.postfieldsize == -1)
      result = Curl_setstropt(&data->set.str[STRING_COPYPOSTFIELDS], argptr);
    else {
      /*
       *  Check that requested length does not overflow the size_t type.
       */

      if((data->set.postfieldsize < 0) ||
         ((sizeof(curl_off_t) != sizeof(size_t)) &&
          (data->set.postfieldsize > (curl_off_t)((size_t)-1))))
        result = CURLE_OUT_OF_MEMORY;
      else {
        /* Allocate even when size == 0. This satisfies the need of possible
           later address compare to detect the COPYPOSTFIELDS mode, and to
           mark that postfields is used rather than read function or form
           data.
        */
        char *p = Curl_memdup0(argptr, (size_t)data->set.postfieldsize);
        if(!p)
          result = CURLE_OUT_OF_MEMORY;
        else {
          free(data->set.str[STRING_COPYPOSTFIELDS]);
          data->set.str[STRING_COPYPOSTFIELDS] = p;
        }
      }
    }

    data->set.postfields = data->set.str[STRING_COPYPOSTFIELDS];
    data->set.method = HTTPREQ_POST;
    break;

  case CURLOPT_POSTFIELDS:
    /*
     * Like above, but use static data instead of copying it.
     */
    data->set.postfields = va_arg(param, void *);
    /* Release old copied data. */
    Curl_safefree(data->set.str[STRING_COPYPOSTFIELDS]);
    data->set.method = HTTPREQ_POST;
    break;

  case CURLOPT_POSTFIELDSIZE:
    /*
     * The size of the POSTFIELD data to prevent libcurl to do strlen() to
     * figure it out. Enables binary posts.
     */
    bigsize = va_arg(param, long);
    if(bigsize < -1)
      return CURLE_BAD_FUNCTION_ARGUMENT;

    if(data->set.postfieldsize < bigsize &&
       data->set.postfields == data->set.str[STRING_COPYPOSTFIELDS]) {
      /* Previous CURLOPT_COPYPOSTFIELDS is no longer valid. */
      Curl_safefree(data->set.str[STRING_COPYPOSTFIELDS]);
      data->set.postfields = NULL;
    }

    data->set.postfieldsize = bigsize;
    break;

  case CURLOPT_POSTFIELDSIZE_LARGE:
    /*
     * The size of the POSTFIELD data to prevent libcurl to do strlen() to
     * figure it out. Enables binary posts.
     */
    bigsize = va_arg(param, curl_off_t);
    if(bigsize < -1)
      return CURLE_BAD_FUNCTION_ARGUMENT;

    if(data->set.postfieldsize < bigsize &&
       data->set.postfields == data->set.str[STRING_COPYPOSTFIELDS]) {
      /* Previous CURLOPT_COPYPOSTFIELDS is no longer valid. */
      Curl_safefree(data->set.str[STRING_COPYPOSTFIELDS]);
      data->set.postfields = NULL;
    }

    data->set.postfieldsize = bigsize;
    break;
#endif
#ifndef CURL_DISABLE_HTTP
  case CURLOPT_AUTOREFERER:
    /*
     * Switch on automatic referer that gets set if curl follows locations.
     */
    data->set.http_auto_referer = (0 != va_arg(param, long));
    break;

  case CURLOPT_ACCEPT_ENCODING:
    /*
     * String to use at the value of Accept-Encoding header.
     *
     * If the encoding is set to "" we use an Accept-Encoding header that
     * encompasses all the encodings we support.
     * If the encoding is set to NULL we don't send an Accept-Encoding header
     * and ignore an received Content-Encoding header.
     *
     */
    argptr = va_arg(param, char *);
    if(argptr && !*argptr) {
      char all[256];
      Curl_all_content_encodings(all, sizeof(all));
      result = Curl_setstropt(&data->set.str[STRING_ENCODING], all);
    }
    else
      result = Curl_setstropt(&data->set.str[STRING_ENCODING], argptr);
    break;

  case CURLOPT_TRANSFER_ENCODING:
    data->set.http_transfer_encoding = (0 != va_arg(param, long));
    break;

  case CURLOPT_FOLLOWLOCATION:
    /*
     * Follow Location: header hints on an HTTP-server.
     */
    data->set.http_follow_location = (0 != va_arg(param, long));
    break;

  case CURLOPT_UNRESTRICTED_AUTH:
    /*
     * Send authentication (user+password) when following locations, even when
     * hostname changed.
     */
    data->set.allow_auth_to_other_hosts = (0 != va_arg(param, long));
    break;

  case CURLOPT_MAXREDIRS:
    /*
     * The maximum amount of hops you allow curl to follow Location:
     * headers. This should mostly be used to detect never-ending loops.
     */
    arg = va_arg(param, long);
    if(arg < -1)
      return CURLE_BAD_FUNCTION_ARGUMENT;
    data->set.maxredirs = arg;
    break;

  case CURLOPT_POSTREDIR:
    /*
     * Set the behavior of POST when redirecting
     * CURL_REDIR_GET_ALL - POST is changed to GET after 301 and 302
     * CURL_REDIR_POST_301 - POST is kept as POST after 301
     * CURL_REDIR_POST_302 - POST is kept as POST after 302
     * CURL_REDIR_POST_303 - POST is kept as POST after 303
     * CURL_REDIR_POST_ALL - POST is kept as POST after 301, 302 and 303
     * other - POST is kept as POST after 301 and 302
     */
    arg = va_arg(param, long);
    if(arg < CURL_REDIR_GET_ALL)
      /* no return error on too high numbers since the bitmask could be
         extended in a future */
      return CURLE_BAD_FUNCTION_ARGUMENT;
    data->set.keep_post = arg & CURL_REDIR_POST_ALL;
    break;

  case CURLOPT_POST:
    /* Does this option serve a purpose anymore? Yes it does, when
       CURLOPT_POSTFIELDS isn't used and the POST data is read off the
       callback! */
    if(va_arg(param, long)) {
      data->set.method = HTTPREQ_POST;
      data->set.opt_no_body = FALSE; /* this is implied */
    }
    else
      data->set.method = HTTPREQ_GET;
    break;

#ifndef CURL_DISABLE_FORM_API
  case CURLOPT_HTTPPOST:
    /*
     * Set to make us do HTTP POST. Legacy API-style.
     */
    data->set.httppost = va_arg(param, struct curl_httppost *);
    data->set.method = HTTPREQ_POST_FORM;
    data->set.opt_no_body = FALSE; /* this is implied */
    Curl_mime_cleanpart(data->state.formp);
    Curl_safefree(data->state.formp);
    data->state.mimepost = NULL;
    break;
#endif

#if !defined(CURL_DISABLE_AWS)
  case CURLOPT_AWS_SIGV4:
    /*
     * String that is merged to some authentication
     * parameters are used by the algorithm.
     */
    result = Curl_setstropt(&data->set.str[STRING_AWS_SIGV4],
                            va_arg(param, char *));
    /*
     * Basic been set by default it need to be unset here
     */
    if(data->set.str[STRING_AWS_SIGV4])
      data->set.httpauth = CURLAUTH_AWS_SIGV4;
    break;
#endif

  case CURLOPT_REFERER:
    /*
     * String to set in the HTTP Referer: field.
     */
    if(data->state.referer_alloc) {
      Curl_safefree(data->state.referer);
      data->state.referer_alloc = FALSE;
    }
    result = Curl_setstropt(&data->set.str[STRING_SET_REFERER],
                            va_arg(param, char *));
    data->state.referer = data->set.str[STRING_SET_REFERER];
    break;

  case CURLOPT_USERAGENT:
    /*
     * String to use in the HTTP User-Agent field
     */
    result = Curl_setstropt(&data->set.str[STRING_USERAGENT],
                            va_arg(param, char *));
    break;

#ifndef CURL_DISABLE_PROXY
  case CURLOPT_PROXYHEADER:
    /*
     * Set a list with proxy headers to use (or replace internals with)
     *
     * Since CURLOPT_HTTPHEADER was the only way to set HTTP headers for a
     * long time we remain doing it this way until CURLOPT_PROXYHEADER is
     * used. As soon as this option has been used, if set to anything but
     * NULL, custom headers for proxies are only picked from this list.
     *
     * Set this option to NULL to restore the previous behavior.
     */
    data->set.proxyheaders = va_arg(param, struct curl_slist *);
    break;
#endif
  case CURLOPT_HEADEROPT:
    /*
     * Set header option.
     */
    arg = va_arg(param, long);
    data->set.sep_headers = !!(arg & CURLHEADER_SEPARATE);
    break;

#if !defined(CURL_DISABLE_COOKIES)
  case CURLOPT_COOKIE:
    /*
     * Cookie string to send to the remote server in the request.
     */
    result = Curl_setstropt(&data->set.str[STRING_COOKIE],
                            va_arg(param, char *));
    break;

  case CURLOPT_COOKIEFILE:
    /*
     * Set cookie file to read and parse. Can be used multiple times.
     */
    argptr = (char *)va_arg(param, void *);
    if(argptr) {
      struct curl_slist *cl;
      /* general protection against mistakes and abuse */
      if(strlen(argptr) > CURL_MAX_INPUT_LENGTH)
        return CURLE_BAD_FUNCTION_ARGUMENT;
      /* append the cookie file name to the list of file names, and deal with
         them later */
      cl = curl_slist_append(data->state.cookielist, argptr);
      if(!cl) {
        curl_slist_free_all(data->state.cookielist);
        data->state.cookielist = NULL;
        return CURLE_OUT_OF_MEMORY;
      }
      data->state.cookielist = cl; /* store the list for later use */
    }
    else {
      /* clear the list of cookie files */
      curl_slist_free_all(data->state.cookielist);
      data->state.cookielist = NULL;

      if(!data->share || !data->share->cookies) {
        /* throw away all existing cookies if this isn't a shared cookie
           container */
        Curl_cookie_clearall(data->cookies);
        Curl_cookie_cleanup(data->cookies);
      }
      /* disable the cookie engine */
      data->cookies = NULL;
    }
    break;

  case CURLOPT_COOKIEJAR:
    /*
     * Set cookie file name to dump all cookies to when we're done.
     */
    result = Curl_setstropt(&data->set.str[STRING_COOKIEJAR],
                            va_arg(param, char *));
    if(!result) {
      /*
       * Activate the cookie parser. This may or may not already
       * have been made.
       */
      struct CookieInfo *newcookies =
        Curl_cookie_init(data, NULL, data->cookies, data->set.cookiesession);
      if(!newcookies)
        result = CURLE_OUT_OF_MEMORY;
      data->cookies = newcookies;
    }
    break;

  case CURLOPT_COOKIESESSION:
    /*
     * Set this option to TRUE to start a new "cookie session". It will
     * prevent the forthcoming read-cookies-from-file actions to accept
     * cookies that are marked as being session cookies, as they belong to a
     * previous session.
     */
    data->set.cookiesession = (0 != va_arg(param, long));
    break;

  case CURLOPT_COOKIELIST:
    argptr = va_arg(param, char *);

    if(!argptr)
      break;

    if(strcasecompare(argptr, "ALL")) {
      /* clear all cookies */
      Curl_share_lock(data, CURL_LOCK_DATA_COOKIE, CURL_LOCK_ACCESS_SINGLE);
      Curl_cookie_clearall(data->cookies);
      Curl_share_unlock(data, CURL_LOCK_DATA_COOKIE);
    }
    else if(strcasecompare(argptr, "SESS")) {
      /* clear session cookies */
      Curl_share_lock(data, CURL_LOCK_DATA_COOKIE, CURL_LOCK_ACCESS_SINGLE);
      Curl_cookie_clearsess(data->cookies);
      Curl_share_unlock(data, CURL_LOCK_DATA_COOKIE);
    }
    else if(strcasecompare(argptr, "FLUSH")) {
      /* flush cookies to file, takes care of the locking */
      Curl_flush_cookies(data, FALSE);
    }
    else if(strcasecompare(argptr, "RELOAD")) {
      /* reload cookies from file */
      Curl_cookie_loadfiles(data);
      break;
    }
    else {
      if(!data->cookies)
        /* if cookie engine was not running, activate it */
        data->cookies = Curl_cookie_init(data, NULL, NULL, TRUE);

      /* general protection against mistakes and abuse */
      if(strlen(argptr) > CURL_MAX_INPUT_LENGTH)
        return CURLE_BAD_FUNCTION_ARGUMENT;
      argptr = strdup(argptr);
      if(!argptr || !data->cookies) {
        result = CURLE_OUT_OF_MEMORY;
        free(argptr);
      }
      else {
        Curl_share_lock(data, CURL_LOCK_DATA_COOKIE, CURL_LOCK_ACCESS_SINGLE);

        if(checkprefix("Set-Cookie:", argptr))
          /* HTTP Header format line */
          Curl_cookie_add(data, data->cookies, TRUE, FALSE, argptr + 11, NULL,
                          NULL, TRUE);

        else
          /* Netscape format line */
          Curl_cookie_add(data, data->cookies, FALSE, FALSE, argptr, NULL,
                          NULL, TRUE);

        Curl_share_unlock(data, CURL_LOCK_DATA_COOKIE);
        free(argptr);
      }
    }

    break;
#endif /* !CURL_DISABLE_COOKIES */

  case CURLOPT_HTTPGET:
    /*
     * Set to force us do HTTP GET
     */
    if(va_arg(param, long)) {
      data->set.method = HTTPREQ_GET;
      data->set.opt_no_body = FALSE; /* this is implied */
    }
    break;

  case CURLOPT_HTTP_VERSION:
    /*
     * This sets a requested HTTP version to be used. The value is one of
     * the listed enums in curl/curl.h.
     */
    arg = va_arg(param, long);
    switch(arg) {
    case CURL_HTTP_VERSION_NONE:
#ifdef USE_HTTP2
      /* TODO: this seems an undesirable quirk to force a behaviour on
       * lower implementations that they should recognize independently? */
      arg = CURL_HTTP_VERSION_2TLS;
#endif
      /* accepted */
      break;
    case CURL_HTTP_VERSION_1_0:
    case CURL_HTTP_VERSION_1_1:
      /* accepted */
      break;
#ifdef USE_HTTP2
    case CURL_HTTP_VERSION_2_0:
    case CURL_HTTP_VERSION_2TLS:
    case CURL_HTTP_VERSION_2_PRIOR_KNOWLEDGE:
      /* accepted */
      break;
#endif
#ifdef USE_HTTP3
    case CURL_HTTP_VERSION_3:
    case CURL_HTTP_VERSION_3ONLY:
      /* accepted */
      break;
#endif
    default:
      /* not accepted */
      if(arg < CURL_HTTP_VERSION_NONE)
        return CURLE_BAD_FUNCTION_ARGUMENT;
      return CURLE_UNSUPPORTED_PROTOCOL;
    }
    data->set.httpwant = (unsigned char)arg;
    break;

  case CURLOPT_EXPECT_100_TIMEOUT_MS:
    /*
     * Time to wait for a response to an HTTP request containing an
     * Expect: 100-continue header before sending the data anyway.
     */
    arg = va_arg(param, long);
    if(arg < 0)
      return CURLE_BAD_FUNCTION_ARGUMENT;
    data->set.expect_100_timeout = arg;
    break;

  case CURLOPT_HTTP09_ALLOWED:
    arg = va_arg(param, unsigned long);
    if(arg > 1L)
      return CURLE_BAD_FUNCTION_ARGUMENT;
#ifdef USE_HYPER
    /* Hyper does not support HTTP/0.9 */
    if(arg)
      return CURLE_BAD_FUNCTION_ARGUMENT;
#else
    data->set.http09_allowed = !!arg;
#endif
    break;

  case CURLOPT_HTTP200ALIASES:
    /*
     * Set a list of aliases for HTTP 200 in response header
     */
    data->set.http200aliases = va_arg(param, struct curl_slist *);
    break;
#endif   /* CURL_DISABLE_HTTP */

#if !defined(CURL_DISABLE_HTTP) || !defined(CURL_DISABLE_SMTP) ||       \
    !defined(CURL_DISABLE_IMAP)
# if !defined(CURL_DISABLE_HTTP) || !defined(CURL_DISABLE_MIME)
  case CURLOPT_HTTPHEADER:
    /*
     * Set a list with HTTP headers to use (or replace internals with)
     */
    data->set.headers = va_arg(param, struct curl_slist *);
    break;
# endif

# ifndef CURL_DISABLE_MIME
  case CURLOPT_MIMEPOST:
    /*
     * Set to make us do MIME POST
     */
    result = Curl_mime_set_subparts(&data->set.mimepost,
                                    va_arg(param, curl_mime *), FALSE);
    if(!result) {
      data->set.method = HTTPREQ_POST_MIME;
      data->set.opt_no_body = FALSE; /* this is implied */
#ifndef CURL_DISABLE_FORM_API
      Curl_mime_cleanpart(data->state.formp);
      Curl_safefree(data->state.formp);
      data->state.mimepost = NULL;
#endif
    }
    break;

  case CURLOPT_MIME_OPTIONS:
    arg = va_arg(param, long);
    data->set.mime_formescape = !!(arg & CURLMIMEOPT_FORMESCAPE);
  break;
# endif
#endif

  case CURLOPT_HTTPAUTH:
    /*
     * Set HTTP Authentication type BITMASK.
     */
  {
    int bitcheck;
    bool authbits;
    unsigned long auth = va_arg(param, unsigned long);

    if(auth == CURLAUTH_NONE) {
      data->set.httpauth = auth;
      break;
    }

    /* the DIGEST_IE bit is only used to set a special marker, for all the
       rest we need to handle it as normal DIGEST */
    data->state.authhost.iestyle = !!(auth & CURLAUTH_DIGEST_IE);

    if(auth & CURLAUTH_DIGEST_IE) {
      auth |= CURLAUTH_DIGEST; /* set standard digest bit */
      auth &= ~CURLAUTH_DIGEST_IE; /* unset ie digest bit */
    }

    /* switch off bits we can't support */
#ifndef USE_NTLM
    auth &= ~CURLAUTH_NTLM;    /* no NTLM support */
#endif
#ifndef USE_SPNEGO
    auth &= ~CURLAUTH_NEGOTIATE; /* no Negotiate (SPNEGO) auth without
                                    GSS-API or SSPI */
#endif

    /* check if any auth bit lower than CURLAUTH_ONLY is still set */
    bitcheck = 0;
    authbits = FALSE;
    while(bitcheck < 31) {
      if(auth & (1UL << bitcheck++)) {
        authbits = TRUE;
        break;
      }
    }
    if(!authbits)
      return CURLE_NOT_BUILT_IN; /* no supported types left! */

    data->set.httpauth = auth;
  }
  break;

  case CURLOPT_CUSTOMREQUEST:
    /*
     * Set a custom string to use as request
     */
    result = Curl_setstropt(&data->set.str[STRING_CUSTOMREQUEST],
                            va_arg(param, char *));

    /* we don't set
       data->set.method = HTTPREQ_CUSTOM;
       here, we continue as if we were using the already set type
       and this just changes the actual request keyword */
    break;

#ifndef CURL_DISABLE_PROXY
  case CURLOPT_HTTPPROXYTUNNEL:
    /*
     * Tunnel operations through the proxy instead of normal proxy use
     */
    data->set.tunnel_thru_httpproxy = (0 != va_arg(param, long));
    break;

  case CURLOPT_PROXYPORT:
    /*
     * Explicitly set HTTP proxy port number.
     */
    arg = va_arg(param, long);
    if((arg < 0) || (arg > 65535))
      return CURLE_BAD_FUNCTION_ARGUMENT;
    data->set.proxyport = (unsigned short)arg;
    break;

  case CURLOPT_PROXYAUTH:
    /*
     * Set HTTP Authentication type BITMASK.
     */
  {
    int bitcheck;
    bool authbits;
    unsigned long auth = va_arg(param, unsigned long);

    if(auth == CURLAUTH_NONE) {
      data->set.proxyauth = auth;
      break;
    }

    /* the DIGEST_IE bit is only used to set a special marker, for all the
       rest we need to handle it as normal DIGEST */
    data->state.authproxy.iestyle = !!(auth & CURLAUTH_DIGEST_IE);

    if(auth & CURLAUTH_DIGEST_IE) {
      auth |= CURLAUTH_DIGEST; /* set standard digest bit */
      auth &= ~CURLAUTH_DIGEST_IE; /* unset ie digest bit */
    }
    /* switch off bits we can't support */
#ifndef USE_NTLM
    auth &= ~CURLAUTH_NTLM;    /* no NTLM support */
#endif
#ifndef USE_SPNEGO
    auth &= ~CURLAUTH_NEGOTIATE; /* no Negotiate (SPNEGO) auth without
                                    GSS-API or SSPI */
#endif

    /* check if any auth bit lower than CURLAUTH_ONLY is still set */
    bitcheck = 0;
    authbits = FALSE;
    while(bitcheck < 31) {
      if(auth & (1UL << bitcheck++)) {
        authbits = TRUE;
        break;
      }
    }
    if(!authbits)
      return CURLE_NOT_BUILT_IN; /* no supported types left! */

    data->set.proxyauth = auth;
  }
  break;

  case CURLOPT_PROXY:
    /*
     * Set proxy server:port to use as proxy.
     *
     * If the proxy is set to "" (and CURLOPT_SOCKS_PROXY is set to "" or NULL)
     * we explicitly say that we don't want to use a proxy
     * (even though there might be environment variables saying so).
     *
     * Setting it to NULL, means no proxy but allows the environment variables
     * to decide for us (if CURLOPT_SOCKS_PROXY setting it to NULL).
     */
    result = Curl_setstropt(&data->set.str[STRING_PROXY],
                            va_arg(param, char *));
    break;

  case CURLOPT_PRE_PROXY:
    /*
     * Set proxy server:port to use as SOCKS proxy.
     *
     * If the proxy is set to "" or NULL we explicitly say that we don't want
     * to use the socks proxy.
     */
    result = Curl_setstropt(&data->set.str[STRING_PRE_PROXY],
                            va_arg(param, char *));
    break;

  case CURLOPT_PROXYTYPE:
    /*
     * Set proxy type.
     */
    arg = va_arg(param, long);
    if((arg < CURLPROXY_HTTP) || (arg > CURLPROXY_SOCKS5_HOSTNAME))
      return CURLE_BAD_FUNCTION_ARGUMENT;
    data->set.proxytype = (unsigned char)(curl_proxytype)arg;
    break;

  case CURLOPT_PROXY_TRANSFER_MODE:
    /*
     * set transfer mode (;type=<a|i>) when doing FTP via an HTTP proxy
     */
    switch(va_arg(param, long)) {
    case 0:
      data->set.proxy_transfer_mode = FALSE;
      break;
    case 1:
      data->set.proxy_transfer_mode = TRUE;
      break;
    default:
      /* reserve other values for future use */
      result = CURLE_BAD_FUNCTION_ARGUMENT;
      break;
    }
    break;

  case CURLOPT_SOCKS5_AUTH:
    data->set.socks5auth = (unsigned char)va_arg(param, unsigned long);
    if(data->set.socks5auth & ~(CURLAUTH_BASIC | CURLAUTH_GSSAPI))
      result = CURLE_NOT_BUILT_IN;
    break;
#endif   /* CURL_DISABLE_PROXY */

#if defined(HAVE_GSSAPI) || defined(USE_WINDOWS_SSPI)
  case CURLOPT_SOCKS5_GSSAPI_NEC:
    /*
     * Set flag for NEC SOCK5 support
     */
    data->set.socks5_gssapi_nec = (0 != va_arg(param, long));
    break;
#endif
#ifndef CURL_DISABLE_PROXY
  case CURLOPT_SOCKS5_GSSAPI_SERVICE:
  case CURLOPT_PROXY_SERVICE_NAME:
    /*
     * Set proxy authentication service name for Kerberos 5 and SPNEGO
     */
    result = Curl_setstropt(&data->set.str[STRING_PROXY_SERVICE_NAME],
                            va_arg(param, char *));
    break;
#endif
  case CURLOPT_SERVICE_NAME:
    /*
     * Set authentication service name for DIGEST-MD5, Kerberos 5 and SPNEGO
     */
    result = Curl_setstropt(&data->set.str[STRING_SERVICE_NAME],
                            va_arg(param, char *));
    break;

  case CURLOPT_HEADERDATA:
    /*
     * Custom pointer to pass the header write callback function
     */
    data->set.writeheader = (void *)va_arg(param, void *);
    break;
  case CURLOPT_ERRORBUFFER:
    /*
     * Error buffer provided by the caller to get the human readable
     * error string in.
     */
    data->set.errorbuffer = va_arg(param, char *);
    break;
  case CURLOPT_WRITEDATA:
    /*
     * FILE pointer to write to. Or possibly
     * used as argument to the write callback.
     */
    data->set.out = va_arg(param, void *);
    break;

#ifdef CURL_LIST_ONLY_PROTOCOL
  case CURLOPT_DIRLISTONLY:
    /*
     * An option that changes the command to one that asks for a list only, no
     * file info details. Used for FTP, POP3 and SFTP.
     */
    data->set.list_only = (0 != va_arg(param, long));
    break;
#endif
  case CURLOPT_APPEND:
    /*
     * We want to upload and append to an existing file. Used for FTP and
     * SFTP.
     */
    data->set.remote_append = (0 != va_arg(param, long));
    break;

#ifndef CURL_DISABLE_FTP
  case CURLOPT_FTP_FILEMETHOD:
    /*
     * How do access files over FTP.
     */
    arg = va_arg(param, long);
    if((arg < CURLFTPMETHOD_DEFAULT) || (arg >= CURLFTPMETHOD_LAST))
      return CURLE_BAD_FUNCTION_ARGUMENT;
    data->set.ftp_filemethod = (unsigned char)arg;
    break;
  case CURLOPT_FTPPORT:
    /*
     * Use FTP PORT, this also specifies which IP address to use
     */
    result = Curl_setstropt(&data->set.str[STRING_FTPPORT],
                            va_arg(param, char *));
    data->set.ftp_use_port = !!(data->set.str[STRING_FTPPORT]);
    break;

  case CURLOPT_FTP_USE_EPRT:
    data->set.ftp_use_eprt = (0 != va_arg(param, long));
    break;

  case CURLOPT_FTP_USE_EPSV:
    data->set.ftp_use_epsv = (0 != va_arg(param, long));
    break;

  case CURLOPT_FTP_USE_PRET:
    data->set.ftp_use_pret = (0 != va_arg(param, long));
    break;

  case CURLOPT_FTP_SSL_CCC:
    arg = va_arg(param, long);
    if((arg < CURLFTPSSL_CCC_NONE) || (arg >= CURLFTPSSL_CCC_LAST))
      return CURLE_BAD_FUNCTION_ARGUMENT;
    data->set.ftp_ccc = (unsigned char)arg;
    break;

  case CURLOPT_FTP_SKIP_PASV_IP:
    /*
     * Enable or disable FTP_SKIP_PASV_IP, which will disable/enable the
     * bypass of the IP address in PASV responses.
     */
    data->set.ftp_skip_ip = (0 != va_arg(param, long));
    break;

  case CURLOPT_FTP_ACCOUNT:
    result = Curl_setstropt(&data->set.str[STRING_FTP_ACCOUNT],
                            va_arg(param, char *));
    break;

  case CURLOPT_FTP_ALTERNATIVE_TO_USER:
    result = Curl_setstropt(&data->set.str[STRING_FTP_ALTERNATIVE_TO_USER],
                            va_arg(param, char *));
    break;

  case CURLOPT_FTPSSLAUTH:
    /*
     * Set a specific auth for FTP-SSL transfers.
     */
    arg = va_arg(param, long);
    if((arg < CURLFTPAUTH_DEFAULT) || (arg >= CURLFTPAUTH_LAST))
      return CURLE_BAD_FUNCTION_ARGUMENT;
    data->set.ftpsslauth = (unsigned char)(curl_ftpauth)arg;
    break;
#ifdef HAVE_GSSAPI
  case CURLOPT_KRBLEVEL:
    /*
     * A string that defines the kerberos security level.
     */
    result = Curl_setstropt(&data->set.str[STRING_KRB_LEVEL],
                            va_arg(param, char *));
    data->set.krb = !!(data->set.str[STRING_KRB_LEVEL]);
    break;
#endif
#endif
#if !defined(CURL_DISABLE_FTP) || defined(USE_SSH)
  case CURLOPT_FTP_CREATE_MISSING_DIRS:
    /*
     * An FTP/SFTP option that modifies an upload to create missing
     * directories on the server.
     */
    arg = va_arg(param, long);
    /* reserve other values for future use */
    if((arg < CURLFTP_CREATE_DIR_NONE) ||
       (arg > CURLFTP_CREATE_DIR_RETRY))
      result = CURLE_BAD_FUNCTION_ARGUMENT;
    else
      data->set.ftp_create_missing_dirs = (unsigned char)arg;
    break;

  case CURLOPT_POSTQUOTE:
    /*
     * List of RAW FTP commands to use after a transfer
     */
    data->set.postquote = va_arg(param, struct curl_slist *);
    break;
  case CURLOPT_PREQUOTE:
    /*
     * List of RAW FTP commands to use prior to RETR (Wesley Laxton)
     */
    data->set.prequote = va_arg(param, struct curl_slist *);
    break;
  case CURLOPT_QUOTE:
    /*
     * List of RAW FTP commands to use before a transfer
     */
    data->set.quote = va_arg(param, struct curl_slist *);
    break;
#endif
  case CURLOPT_READDATA:
    /*
     * FILE pointer to read the file to be uploaded from. Or possibly
     * used as argument to the read callback.
     */
    data->set.in_set = va_arg(param, void *);
    break;
  case CURLOPT_INFILESIZE:
    /*
     * If known, this should inform curl about the file size of the
     * to-be-uploaded file.
     */
    arg = va_arg(param, long);
    if(arg < -1)
      return CURLE_BAD_FUNCTION_ARGUMENT;
    data->set.filesize = arg;
    break;
  case CURLOPT_INFILESIZE_LARGE:
    /*
     * If known, this should inform curl about the file size of the
     * to-be-uploaded file.
     */
    bigsize = va_arg(param, curl_off_t);
    if(bigsize < -1)
      return CURLE_BAD_FUNCTION_ARGUMENT;
    data->set.filesize = bigsize;
    break;
  case CURLOPT_LOW_SPEED_LIMIT:
    /*
     * The low speed limit that if transfers are below this for
     * CURLOPT_LOW_SPEED_TIME, the transfer is aborted.
     */
    arg = va_arg(param, long);
    if(arg < 0)
      return CURLE_BAD_FUNCTION_ARGUMENT;
    data->set.low_speed_limit = arg;
    break;
  case CURLOPT_MAX_SEND_SPEED_LARGE:
    /*
     * When transfer uploads are faster then CURLOPT_MAX_SEND_SPEED_LARGE
     * bytes per second the transfer is throttled..
     */
    bigsize = va_arg(param, curl_off_t);
    if(bigsize < 0)
      return CURLE_BAD_FUNCTION_ARGUMENT;
    data->set.max_send_speed = bigsize;
    break;
  case CURLOPT_MAX_RECV_SPEED_LARGE:
    /*
     * When receiving data faster than CURLOPT_MAX_RECV_SPEED_LARGE bytes per
     * second the transfer is throttled..
     */
    bigsize = va_arg(param, curl_off_t);
    if(bigsize < 0)
      return CURLE_BAD_FUNCTION_ARGUMENT;
    data->set.max_recv_speed = bigsize;
    break;
  case CURLOPT_LOW_SPEED_TIME:
    /*
     * The low speed time that if transfers are below the set
     * CURLOPT_LOW_SPEED_LIMIT during this time, the transfer is aborted.
     */
    arg = va_arg(param, long);
    if(arg < 0)
      return CURLE_BAD_FUNCTION_ARGUMENT;
    data->set.low_speed_time = arg;
    break;
  case CURLOPT_CURLU:
    /*
     * pass CURLU to set URL
     */
    data->set.uh = va_arg(param, CURLU *);
    break;
  case CURLOPT_URL:
    /*
     * The URL to fetch.
     */
    if(data->state.url_alloc) {
      /* the already set URL is allocated, free it first! */
      Curl_safefree(data->state.url);
      data->state.url_alloc = FALSE;
    }
    result = Curl_setstropt(&data->set.str[STRING_SET_URL],
                            va_arg(param, char *));
    data->state.url = data->set.str[STRING_SET_URL];
    break;
  case CURLOPT_PORT:
    /*
     * The port number to use when getting the URL. 0 disables it.
     */
    arg = va_arg(param, long);
    if((arg < 0) || (arg > 65535))
      return CURLE_BAD_FUNCTION_ARGUMENT;
    data->set.use_port = (unsigned short)arg;
    break;
  case CURLOPT_TIMEOUT:
    /*
     * The maximum time you allow curl to use for a single transfer
     * operation.
     */
    arg = va_arg(param, long);
    if((arg >= 0) && (arg <= (INT_MAX/1000)))
      data->set.timeout = (unsigned int)arg * 1000;
    else
      return CURLE_BAD_FUNCTION_ARGUMENT;
    break;

  case CURLOPT_TIMEOUT_MS:
    uarg = va_arg(param, unsigned long);
    if(uarg > UINT_MAX)
      uarg = UINT_MAX;
    data->set.timeout = (unsigned int)uarg;
    break;

  case CURLOPT_CONNECTTIMEOUT:
    /*
     * The maximum time you allow curl to use to connect.
     */
    arg = va_arg(param, long);
    if((arg >= 0) && (arg <= (INT_MAX/1000)))
      data->set.connecttimeout = (unsigned int)arg * 1000;
    else
      return CURLE_BAD_FUNCTION_ARGUMENT;
    break;

  case CURLOPT_CONNECTTIMEOUT_MS:
    uarg = va_arg(param, unsigned long);
    if(uarg > UINT_MAX)
      uarg = UINT_MAX;
    data->set.connecttimeout = (unsigned int)uarg;
    break;

#ifndef CURL_DISABLE_FTP
  case CURLOPT_ACCEPTTIMEOUT_MS:
    /*
     * The maximum time for curl to wait for FTP server connect
     */
    uarg = va_arg(param, unsigned long);
    if(uarg > UINT_MAX)
      uarg = UINT_MAX;
    data->set.accepttimeout = (unsigned int)uarg;
    break;
#endif

  case CURLOPT_USERPWD:
    /*
     * user:password to use in the operation
     */
    result = setstropt_userpwd(va_arg(param, char *),
                               &data->set.str[STRING_USERNAME],
                               &data->set.str[STRING_PASSWORD]);
    break;

  case CURLOPT_USERNAME:
    /*
     * authentication user name to use in the operation
     */
    result = Curl_setstropt(&data->set.str[STRING_USERNAME],
                            va_arg(param, char *));
    break;
  case CURLOPT_PASSWORD:
    /*
     * authentication password to use in the operation
     */
    result = Curl_setstropt(&data->set.str[STRING_PASSWORD],
                            va_arg(param, char *));
    break;

  case CURLOPT_LOGIN_OPTIONS:
    /*
     * authentication options to use in the operation
     */
    result = Curl_setstropt(&data->set.str[STRING_OPTIONS],
                            va_arg(param, char *));
    break;

  case CURLOPT_XOAUTH2_BEARER:
    /*
     * OAuth 2.0 bearer token to use in the operation
     */
    result = Curl_setstropt(&data->set.str[STRING_BEARER],
                            va_arg(param, char *));
    break;

  case CURLOPT_RESOLVE:
    /*
     * List of HOST:PORT:[addresses] strings to populate the DNS cache with
     * Entries added this way will remain in the cache until explicitly
     * removed or the handle is cleaned up.
     *
     * Prefix the HOST with plus sign (+) to have the entry expire just like
     * automatically added entries.
     *
     * Prefix the HOST with dash (-) to _remove_ the entry from the cache.
     *
     * This API can remove any entry from the DNS cache, but only entries
     * that aren't actually in use right now will be pruned immediately.
     */
    data->set.resolve = va_arg(param, struct curl_slist *);
    data->state.resolve = data->set.resolve;
    break;
  case CURLOPT_PROGRESSFUNCTION:
    /*
     * Progress callback function
     */
    data->set.fprogress = va_arg(param, curl_progress_callback);
    if(data->set.fprogress)
      data->progress.callback = TRUE; /* no longer internal */
    else
      data->progress.callback = FALSE; /* NULL enforces internal */
    break;

  case CURLOPT_XFERINFOFUNCTION:
    /*
     * Transfer info callback function
     */
    data->set.fxferinfo = va_arg(param, curl_xferinfo_callback);
    if(data->set.fxferinfo)
      data->progress.callback = TRUE; /* no longer internal */
    else
      data->progress.callback = FALSE; /* NULL enforces internal */

    break;

  case CURLOPT_PROGRESSDATA:
    /*
     * Custom client data to pass to the progress callback
     */
    data->set.progress_client = va_arg(param, void *);
    break;

#ifndef CURL_DISABLE_PROXY
  case CURLOPT_PROXYUSERPWD: {
    /*
     * user:password needed to use the proxy
     */
    char *u = NULL;
    char *p = NULL;
    result = setstropt_userpwd(va_arg(param, char *), &u, &p);

    /* URL decode the components */
    if(!result && u)
      result = Curl_urldecode(u, 0, &data->set.str[STRING_PROXYUSERNAME], NULL,
                              REJECT_ZERO);
    if(!result && p)
      result = Curl_urldecode(p, 0, &data->set.str[STRING_PROXYPASSWORD], NULL,
                              REJECT_ZERO);
    free(u);
    free(p);
  }
    break;
  case CURLOPT_PROXYUSERNAME:
    /*
     * authentication user name to use in the operation
     */
    result = Curl_setstropt(&data->set.str[STRING_PROXYUSERNAME],
                            va_arg(param, char *));
    break;
  case CURLOPT_PROXYPASSWORD:
    /*
     * authentication password to use in the operation
     */
    result = Curl_setstropt(&data->set.str[STRING_PROXYPASSWORD],
                            va_arg(param, char *));
    break;
  case CURLOPT_NOPROXY:
    /*
     * proxy exception list
     */
    result = Curl_setstropt(&data->set.str[STRING_NOPROXY],
                            va_arg(param, char *));
    break;
#endif

  case CURLOPT_RANGE:
    /*
     * What range of the file you want to transfer
     */
    result = Curl_setstropt(&data->set.str[STRING_SET_RANGE],
                            va_arg(param, char *));
    break;
  case CURLOPT_RESUME_FROM:
    /*
     * Resume transfer at the given file position
     */
    arg = va_arg(param, long);
    if(arg < -1)
      return CURLE_BAD_FUNCTION_ARGUMENT;
    data->set.set_resume_from = arg;
    break;
  case CURLOPT_RESUME_FROM_LARGE:
    /*
     * Resume transfer at the given file position
     */
    bigsize = va_arg(param, curl_off_t);
    if(bigsize < -1)
      return CURLE_BAD_FUNCTION_ARGUMENT;
    data->set.set_resume_from = bigsize;
    break;
  case CURLOPT_DEBUGFUNCTION:
    /*
     * stderr write callback.
     */
    data->set.fdebug = va_arg(param, curl_debug_callback);
    /*
     * if the callback provided is NULL, it'll use the default callback
     */
    break;
  case CURLOPT_DEBUGDATA:
    /*
     * Set to a void * that should receive all error writes. This
     * defaults to CURLOPT_STDERR for normal operations.
     */
    data->set.debugdata = va_arg(param, void *);
    break;
  case CURLOPT_STDERR:
    /*
     * Set to a FILE * that should receive all error writes. This
     * defaults to stderr for normal operations.
     */
    data->set.err = va_arg(param, FILE *);
    if(!data->set.err)
      data->set.err = stderr;
    break;
  case CURLOPT_HEADERFUNCTION:
    /*
     * Set header write callback
     */
    data->set.fwrite_header = va_arg(param, curl_write_callback);
    break;
  case CURLOPT_WRITEFUNCTION:
    /*
     * Set data write callback
     */
    data->set.fwrite_func = va_arg(param, curl_write_callback);
    if(!data->set.fwrite_func)
      /* When set to NULL, reset to our internal default function */
      data->set.fwrite_func = (curl_write_callback)fwrite;
    break;
  case CURLOPT_READFUNCTION:
    /*
     * Read data callback
     */
    data->set.fread_func_set = va_arg(param, curl_read_callback);
    if(!data->set.fread_func_set) {
      data->set.is_fread_set = 0;
      /* When set to NULL, reset to our internal default function */
      data->set.fread_func_set = (curl_read_callback)fread;
    }
    else
      data->set.is_fread_set = 1;
    break;
  case CURLOPT_SEEKFUNCTION:
    /*
     * Seek callback. Might be NULL.
     */
    data->set.seek_func = va_arg(param, curl_seek_callback);
    break;
  case CURLOPT_SEEKDATA:
    /*
     * Seek control callback. Might be NULL.
     */
    data->set.seek_client = va_arg(param, void *);
    break;
  case CURLOPT_IOCTLFUNCTION:
    /*
     * I/O control callback. Might be NULL.
     */
    data->set.ioctl_func = va_arg(param, curl_ioctl_callback);
    break;
  case CURLOPT_IOCTLDATA:
    /*
     * I/O control data pointer. Might be NULL.
     */
    data->set.ioctl_client = va_arg(param, void *);
    break;
  case CURLOPT_SSLCERT:
    /*
     * String that holds file name of the SSL certificate to use
     */
    result = Curl_setstropt(&data->set.str[STRING_CERT],
                            va_arg(param, char *));
    break;
  case CURLOPT_SSLCERT_BLOB:
    /*
     * Blob that holds file content of the SSL certificate to use
     */
    result = Curl_setblobopt(&data->set.blobs[BLOB_CERT],
                             va_arg(param, struct curl_blob *));
    break;
#ifndef CURL_DISABLE_PROXY
  case CURLOPT_PROXY_SSLCERT:
    /*
     * String that holds file name of the SSL certificate to use for proxy
     */
    result = Curl_setstropt(&data->set.str[STRING_CERT_PROXY],
                            va_arg(param, char *));
    break;
  case CURLOPT_PROXY_SSLCERT_BLOB:
    /*
     * Blob that holds file content of the SSL certificate to use for proxy
     */
    result = Curl_setblobopt(&data->set.blobs[BLOB_CERT_PROXY],
                             va_arg(param, struct curl_blob *));
    break;
#endif
  case CURLOPT_SSLCERTTYPE:
    /*
     * String that holds file type of the SSL certificate to use
     */
    result = Curl_setstropt(&data->set.str[STRING_CERT_TYPE],
                            va_arg(param, char *));
    break;
#ifndef CURL_DISABLE_PROXY
  case CURLOPT_PROXY_SSLCERTTYPE:
    /*
     * String that holds file type of the SSL certificate to use for proxy
     */
    result = Curl_setstropt(&data->set.str[STRING_CERT_TYPE_PROXY],
                            va_arg(param, char *));
    break;
#endif
  case CURLOPT_SSLKEY:
    /*
     * String that holds file name of the SSL key to use
     */
    result = Curl_setstropt(&data->set.str[STRING_KEY],
                            va_arg(param, char *));
    break;
  case CURLOPT_SSLKEY_BLOB:
    /*
     * Blob that holds file content of the SSL key to use
     */
    result = Curl_setblobopt(&data->set.blobs[BLOB_KEY],
                             va_arg(param, struct curl_blob *));
    break;
#ifndef CURL_DISABLE_PROXY
  case CURLOPT_PROXY_SSLKEY:
    /*
     * String that holds file name of the SSL key to use for proxy
     */
    result = Curl_setstropt(&data->set.str[STRING_KEY_PROXY],
                            va_arg(param, char *));
    break;
  case CURLOPT_PROXY_SSLKEY_BLOB:
    /*
     * Blob that holds file content of the SSL key to use for proxy
     */
    result = Curl_setblobopt(&data->set.blobs[BLOB_KEY_PROXY],
                             va_arg(param, struct curl_blob *));
    break;
#endif
  case CURLOPT_SSLKEYTYPE:
    /*
     * String that holds file type of the SSL key to use
     */
    result = Curl_setstropt(&data->set.str[STRING_KEY_TYPE],
                            va_arg(param, char *));
    break;
#ifndef CURL_DISABLE_PROXY
  case CURLOPT_PROXY_SSLKEYTYPE:
    /*
     * String that holds file type of the SSL key to use for proxy
     */
    result = Curl_setstropt(&data->set.str[STRING_KEY_TYPE_PROXY],
                            va_arg(param, char *));
    break;
#endif
  case CURLOPT_KEYPASSWD:
    /*
     * String that holds the SSL or SSH private key password.
     */
    result = Curl_setstropt(&data->set.str[STRING_KEY_PASSWD],
                            va_arg(param, char *));
    break;
#ifndef CURL_DISABLE_PROXY
  case CURLOPT_PROXY_KEYPASSWD:
    /*
     * String that holds the SSL private key password for proxy.
     */
    result = Curl_setstropt(&data->set.str[STRING_KEY_PASSWD_PROXY],
                            va_arg(param, char *));
    break;
#endif
  case CURLOPT_SSLENGINE:
    /*
     * String that holds the SSL crypto engine.
     */
    argptr = va_arg(param, char *);
    if(argptr && argptr[0]) {
      result = Curl_setstropt(&data->set.str[STRING_SSL_ENGINE], argptr);
      if(!result) {
        result = Curl_ssl_set_engine(data, argptr);
      }
    }
    break;

  case CURLOPT_SSLENGINE_DEFAULT:
    /*
     * flag to set engine as default.
     */
    Curl_safefree(data->set.str[STRING_SSL_ENGINE]);
    result = Curl_ssl_set_engine_default(data);
    break;
  case CURLOPT_CRLF:
    /*
     * Kludgy option to enable CRLF conversions. Subject for removal.
     */
    data->set.crlf = (0 != va_arg(param, long));
    break;
#ifndef CURL_DISABLE_PROXY
  case CURLOPT_HAPROXYPROTOCOL:
    /*
     * Set to send the HAProxy Proxy Protocol header
     */
    data->set.haproxyprotocol = (0 != va_arg(param, long));
    break;
  case CURLOPT_HAPROXY_CLIENT_IP:
    /*
     * Set the client IP to send through HAProxy PROXY protocol
     */
    result = Curl_setstropt(&data->set.str[STRING_HAPROXY_CLIENT_IP],
                            va_arg(param, char *));
    /* We enable implicitly the HAProxy protocol if we use this flag. */
    data->set.haproxyprotocol = TRUE;
    break;
#endif
  case CURLOPT_INTERFACE:
    /*
     * Set what interface or address/hostname to bind the socket to when
     * performing an operation and thus what from-IP your connection will use.
     */
    result = Curl_setstropt(&data->set.str[STRING_DEVICE],
                            va_arg(param, char *));
    break;
#ifndef CURL_DISABLE_BINDLOCAL
  case CURLOPT_LOCALPORT:
    /*
     * Set what local port to bind the socket to when performing an operation.
     */
    arg = va_arg(param, long);
    if((arg < 0) || (arg > 65535))
      return CURLE_BAD_FUNCTION_ARGUMENT;
    data->set.localport = curlx_sltous(arg);
    break;
  case CURLOPT_LOCALPORTRANGE:
    /*
     * Set number of local ports to try, starting with CURLOPT_LOCALPORT.
     */
    arg = va_arg(param, long);
    if((arg < 0) || (arg > 65535))
      return CURLE_BAD_FUNCTION_ARGUMENT;
    data->set.localportrange = curlx_sltous(arg);
    break;
#endif
  case CURLOPT_GSSAPI_DELEGATION:
    /*
     * GSS-API credential delegation bitmask
     */
    uarg = va_arg(param, unsigned long);
    data->set.gssapi_delegation = (unsigned char)uarg&
      (CURLGSSAPI_DELEGATION_POLICY_FLAG|CURLGSSAPI_DELEGATION_FLAG);
    break;
  case CURLOPT_SSL_VERIFYPEER:
    /*
     * Enable peer SSL verifying.
     */
    data->set.ssl.primary.verifypeer = (0 != va_arg(param, long));

    /* Update the current connection ssl_config. */
    Curl_ssl_conn_config_update(data, FALSE);
    break;
#ifndef CURL_DISABLE_DOH
  case CURLOPT_DOH_SSL_VERIFYPEER:
    /*
     * Enable peer SSL verifying for DoH.
     */
    data->set.doh_verifypeer = (0 != va_arg(param, long));
    break;
#endif
#ifndef CURL_DISABLE_PROXY
  case CURLOPT_PROXY_SSL_VERIFYPEER:
    /*
     * Enable peer SSL verifying for proxy.
     */
    data->set.proxy_ssl.primary.verifypeer =
      (0 != va_arg(param, long))?TRUE:FALSE;

    /* Update the current connection proxy_ssl_config. */
    Curl_ssl_conn_config_update(data, TRUE);
    break;
#endif
  case CURLOPT_SSL_VERIFYHOST:
    /*
     * Enable verification of the host name in the peer certificate
     */
    arg = va_arg(param, long);

    /* Obviously people are not reading documentation and too many thought
       this argument took a boolean when it wasn't and misused it.
       Treat 1 and 2 the same */
    data->set.ssl.primary.verifyhost = !!(arg & 3);

    /* Update the current connection ssl_config. */
    Curl_ssl_conn_config_update(data, FALSE);
    break;
#ifndef CURL_DISABLE_DOH
  case CURLOPT_DOH_SSL_VERIFYHOST:
    /*
     * Enable verification of the host name in the peer certificate for DoH
     */
    arg = va_arg(param, long);

    /* Treat both 1 and 2 as TRUE */
    data->set.doh_verifyhost = !!(arg & 3);
    break;
#endif
#ifndef CURL_DISABLE_PROXY
  case CURLOPT_PROXY_SSL_VERIFYHOST:
    /*
     * Enable verification of the host name in the peer certificate for proxy
     */
    arg = va_arg(param, long);

    /* Treat both 1 and 2 as TRUE */
    data->set.proxy_ssl.primary.verifyhost = (bool)((arg & 3)?TRUE:FALSE);
    /* Update the current connection proxy_ssl_config. */
    Curl_ssl_conn_config_update(data, TRUE);
    break;
#endif
  case CURLOPT_SSL_VERIFYSTATUS:
    /*
     * Enable certificate status verifying.
     */
    if(!Curl_ssl_cert_status_request()) {
      result = CURLE_NOT_BUILT_IN;
      break;
    }

    data->set.ssl.primary.verifystatus = (0 != va_arg(param, long));

    /* Update the current connection ssl_config. */
    Curl_ssl_conn_config_update(data, FALSE);
    break;
#ifndef CURL_DISABLE_DOH
  case CURLOPT_DOH_SSL_VERIFYSTATUS:
    /*
     * Enable certificate status verifying for DoH.
     */
    if(!Curl_ssl_cert_status_request()) {
      result = CURLE_NOT_BUILT_IN;
      break;
    }

    data->set.doh_verifystatus = (0 != va_arg(param, long));
    break;
#endif
  case CURLOPT_SSL_CTX_FUNCTION:
    /*
     * Set a SSL_CTX callback
     */
#ifdef USE_SSL
    if(Curl_ssl_supports(data, SSLSUPP_SSL_CTX))
      data->set.ssl.fsslctx = va_arg(param, curl_ssl_ctx_callback);
    else
#endif
      result = CURLE_NOT_BUILT_IN;
    break;
  case CURLOPT_SSL_CTX_DATA:
    /*
     * Set a SSL_CTX callback parameter pointer
     */
#ifdef USE_SSL
    if(Curl_ssl_supports(data, SSLSUPP_SSL_CTX))
      data->set.ssl.fsslctxp = va_arg(param, void *);
    else
#endif
      result = CURLE_NOT_BUILT_IN;
    break;
  case CURLOPT_SSL_FALSESTART:
    /*
     * Enable TLS false start.
     */
    if(!Curl_ssl_false_start(data)) {
      result = CURLE_NOT_BUILT_IN;
      break;
    }

    data->set.ssl.falsestart = (0 != va_arg(param, long));
    break;
  case CURLOPT_CERTINFO:
#ifdef USE_SSL
    if(Curl_ssl_supports(data, SSLSUPP_CERTINFO))
      data->set.ssl.certinfo = (0 != va_arg(param, long));
    else
#endif
      result = CURLE_NOT_BUILT_IN;
    break;
  case CURLOPT_PINNEDPUBLICKEY:
    /*
     * Set pinned public key for SSL connection.
     * Specify file name of the public key in DER format.
     */
#ifdef USE_SSL
    if(Curl_ssl_supports(data, SSLSUPP_PINNEDPUBKEY))
      result = Curl_setstropt(&data->set.str[STRING_SSL_PINNEDPUBLICKEY],
                              va_arg(param, char *));
    else
#endif
      result = CURLE_NOT_BUILT_IN;
    break;
#ifndef CURL_DISABLE_PROXY
  case CURLOPT_PROXY_PINNEDPUBLICKEY:
    /*
     * Set pinned public key for SSL connection.
     * Specify file name of the public key in DER format.
     */
#ifdef USE_SSL
    if(Curl_ssl_supports(data, SSLSUPP_PINNEDPUBKEY))
      result = Curl_setstropt(&data->set.str[STRING_SSL_PINNEDPUBLICKEY_PROXY],
                              va_arg(param, char *));
    else
#endif
      result = CURLE_NOT_BUILT_IN;
    break;
#endif
  case CURLOPT_CAINFO:
    /*
     * Set CA info for SSL connection. Specify file name of the CA certificate
     */
    result = Curl_setstropt(&data->set.str[STRING_SSL_CAFILE],
                            va_arg(param, char *));
    break;
  case CURLOPT_CAINFO_BLOB:
    /*
     * Blob that holds CA info for SSL connection.
     * Specify entire PEM of the CA certificate
     */
#ifdef USE_SSL
    if(Curl_ssl_supports(data, SSLSUPP_CAINFO_BLOB)) {
      result = Curl_setblobopt(&data->set.blobs[BLOB_CAINFO],
                               va_arg(param, struct curl_blob *));
      break;
    }
    else
#endif
      return CURLE_NOT_BUILT_IN;
#ifndef CURL_DISABLE_PROXY
  case CURLOPT_PROXY_CAINFO:
    /*
     * Set CA info SSL connection for proxy. Specify file name of the
     * CA certificate
     */
    result = Curl_setstropt(&data->set.str[STRING_SSL_CAFILE_PROXY],
                            va_arg(param, char *));
    break;
  case CURLOPT_PROXY_CAINFO_BLOB:
    /*
     * Blob that holds CA info for SSL connection proxy.
     * Specify entire PEM of the CA certificate
     */
#ifdef USE_SSL
    if(Curl_ssl_supports(data, SSLSUPP_CAINFO_BLOB)) {
      result = Curl_setblobopt(&data->set.blobs[BLOB_CAINFO_PROXY],
                               va_arg(param, struct curl_blob *));
      break;
    }
    else
#endif
      return CURLE_NOT_BUILT_IN;
#endif
  case CURLOPT_CAPATH:
    /*
     * Set CA path info for SSL connection. Specify directory name of the CA
     * certificates which have been prepared using openssl c_rehash utility.
     */
#ifdef USE_SSL
    if(Curl_ssl_supports(data, SSLSUPP_CA_PATH))
      /* This does not work on windows. */
      result = Curl_setstropt(&data->set.str[STRING_SSL_CAPATH],
                              va_arg(param, char *));
    else
#endif
      result = CURLE_NOT_BUILT_IN;
    break;
#ifndef CURL_DISABLE_PROXY
  case CURLOPT_PROXY_CAPATH:
    /*
     * Set CA path info for SSL connection proxy. Specify directory name of the
     * CA certificates which have been prepared using openssl c_rehash utility.
     */
#ifdef USE_SSL
    if(Curl_ssl_supports(data, SSLSUPP_CA_PATH))
      /* This does not work on windows. */
      result = Curl_setstropt(&data->set.str[STRING_SSL_CAPATH_PROXY],
                              va_arg(param, char *));
    else
#endif
      result = CURLE_NOT_BUILT_IN;
    break;
#endif
  case CURLOPT_CRLFILE:
    /*
     * Set CRL file info for SSL connection. Specify file name of the CRL
     * to check certificates revocation
     */
    result = Curl_setstropt(&data->set.str[STRING_SSL_CRLFILE],
                            va_arg(param, char *));
    break;
#ifndef CURL_DISABLE_PROXY
  case CURLOPT_PROXY_CRLFILE:
    /*
     * Set CRL file info for SSL connection for proxy. Specify file name of the
     * CRL to check certificates revocation
     */
    result = Curl_setstropt(&data->set.str[STRING_SSL_CRLFILE_PROXY],
                            va_arg(param, char *));
    break;
#endif
  case CURLOPT_ISSUERCERT:
    /*
     * Set Issuer certificate file
     * to check certificates issuer
     */
    result = Curl_setstropt(&data->set.str[STRING_SSL_ISSUERCERT],
                            va_arg(param, char *));
    break;
  case CURLOPT_ISSUERCERT_BLOB:
    /*
     * Blob that holds Issuer certificate to check certificates issuer
     */
    result = Curl_setblobopt(&data->set.blobs[BLOB_SSL_ISSUERCERT],
                             va_arg(param, struct curl_blob *));
    break;
#ifndef CURL_DISABLE_PROXY
  case CURLOPT_PROXY_ISSUERCERT:
    /*
     * Set Issuer certificate file
     * to check certificates issuer
     */
    result = Curl_setstropt(&data->set.str[STRING_SSL_ISSUERCERT_PROXY],
                            va_arg(param, char *));
    break;
  case CURLOPT_PROXY_ISSUERCERT_BLOB:
    /*
     * Blob that holds Issuer certificate to check certificates issuer
     */
    result = Curl_setblobopt(&data->set.blobs[BLOB_SSL_ISSUERCERT_PROXY],
                             va_arg(param, struct curl_blob *));
    break;
#endif
#ifndef CURL_DISABLE_TELNET
  case CURLOPT_TELNETOPTIONS:
    /*
     * Set a linked list of telnet options
     */
    data->set.telnet_options = va_arg(param, struct curl_slist *);
    break;
#endif
  case CURLOPT_BUFFERSIZE:
    /*
     * The application kindly asks for a differently sized receive buffer.
     * If it seems reasonable, we'll use it.
     */
    arg = va_arg(param, long);

    if(arg > READBUFFER_MAX)
      arg = READBUFFER_MAX;
    else if(arg < 1)
      arg = READBUFFER_SIZE;
    else if(arg < READBUFFER_MIN)
      arg = READBUFFER_MIN;

    data->set.buffer_size = (unsigned int)arg;
    break;

  case CURLOPT_UPLOAD_BUFFERSIZE:
    /*
     * The application kindly asks for a differently sized upload buffer.
     * Cap it to sensible.
     */
    arg = va_arg(param, long);

    if(arg > UPLOADBUFFER_MAX)
      arg = UPLOADBUFFER_MAX;
    else if(arg < UPLOADBUFFER_MIN)
      arg = UPLOADBUFFER_MIN;

    data->set.upload_buffer_size = (unsigned int)arg;
    break;

  case CURLOPT_NOSIGNAL:
    /*
     * The application asks not to set any signal() or alarm() handlers,
     * even when using a timeout.
     */
    data->set.no_signal = (0 != va_arg(param, long));
    break;

  case CURLOPT_SHARE:
  {
    struct Curl_share *set;
    set = va_arg(param, struct Curl_share *);

    /* disconnect from old share, if any */
    if(data->share) {
      Curl_share_lock(data, CURL_LOCK_DATA_SHARE, CURL_LOCK_ACCESS_SINGLE);

      if(data->dns.hostcachetype == HCACHE_SHARED) {
        data->dns.hostcache = NULL;
        data->dns.hostcachetype = HCACHE_NONE;
      }

#if !defined(CURL_DISABLE_HTTP) && !defined(CURL_DISABLE_COOKIES)
      if(data->share->cookies == data->cookies)
        data->cookies = NULL;
#endif

#ifndef CURL_DISABLE_HSTS
      if(data->share->hsts == data->hsts)
        data->hsts = NULL;
#endif
#ifdef USE_SSL
      if(data->share->sslsession == data->state.session)
        data->state.session = NULL;
#endif
#ifdef USE_LIBPSL
      if(data->psl == &data->share->psl)
        data->psl = data->multi? &data->multi->psl: NULL;
#endif

      data->share->dirty--;

      Curl_share_unlock(data, CURL_LOCK_DATA_SHARE);
      data->share = NULL;
    }

    if(GOOD_SHARE_HANDLE(set))
      /* use new share if it set */
      data->share = set;
    if(data->share) {

      Curl_share_lock(data, CURL_LOCK_DATA_SHARE, CURL_LOCK_ACCESS_SINGLE);

      data->share->dirty++;

      if(data->share->specifier & (1<< CURL_LOCK_DATA_DNS)) {
        /* use shared host cache */
        data->dns.hostcache = &data->share->hostcache;
        data->dns.hostcachetype = HCACHE_SHARED;
      }
#if !defined(CURL_DISABLE_HTTP) && !defined(CURL_DISABLE_COOKIES)
      if(data->share->cookies) {
        /* use shared cookie list, first free own one if any */
        Curl_cookie_cleanup(data->cookies);
        /* enable cookies since we now use a share that uses cookies! */
        data->cookies = data->share->cookies;
      }
#endif   /* CURL_DISABLE_HTTP */
#ifndef CURL_DISABLE_HSTS
      if(data->share->hsts) {
        /* first free the private one if any */
        Curl_hsts_cleanup(&data->hsts);
        data->hsts = data->share->hsts;
      }
#endif
#ifdef USE_SSL
      if(data->share->sslsession) {
        data->set.general_ssl.max_ssl_sessions = data->share->max_ssl_sessions;
        data->state.session = data->share->sslsession;
      }
#endif
#ifdef USE_LIBPSL
      if(data->share->specifier & (1 << CURL_LOCK_DATA_PSL))
        data->psl = &data->share->psl;
#endif

      Curl_share_unlock(data, CURL_LOCK_DATA_SHARE);
    }
    /* check for host cache not needed,
     * it will be done by curl_easy_perform */
  }
  break;

  case CURLOPT_PRIVATE:
    /*
     * Set private data pointer.
     */
    data->set.private_data = va_arg(param, void *);
    break;

  case CURLOPT_MAXFILESIZE:
    /*
     * Set the maximum size of a file to download.
     */
    arg = va_arg(param, long);
    if(arg < 0)
      return CURLE_BAD_FUNCTION_ARGUMENT;
    data->set.max_filesize = arg;
    break;

#ifdef USE_SSL
  case CURLOPT_USE_SSL:
    /*
     * Make transfers attempt to use SSL/TLS.
     */
    arg = va_arg(param, long);
    if((arg < CURLUSESSL_NONE) || (arg >= CURLUSESSL_LAST))
      return CURLE_BAD_FUNCTION_ARGUMENT;
    data->set.use_ssl = (unsigned char)arg;
    break;

  case CURLOPT_SSL_OPTIONS:
    arg = va_arg(param, long);
    data->set.ssl.primary.ssl_options = (unsigned char)(arg & 0xff);
    data->set.ssl.enable_beast = !!(arg & CURLSSLOPT_ALLOW_BEAST);
    data->set.ssl.no_revoke = !!(arg & CURLSSLOPT_NO_REVOKE);
    data->set.ssl.no_partialchain = !!(arg & CURLSSLOPT_NO_PARTIALCHAIN);
    data->set.ssl.revoke_best_effort = !!(arg & CURLSSLOPT_REVOKE_BEST_EFFORT);
    data->set.ssl.native_ca_store = !!(arg & CURLSSLOPT_NATIVE_CA);
    data->set.ssl.auto_client_cert = !!(arg & CURLSSLOPT_AUTO_CLIENT_CERT);
    /* If a setting is added here it should also be added in dohprobe()
       which sets its own CURLOPT_SSL_OPTIONS based on these settings. */
    break;

#ifndef CURL_DISABLE_PROXY
  case CURLOPT_PROXY_SSL_OPTIONS:
    arg = va_arg(param, long);
    data->set.proxy_ssl.primary.ssl_options = (unsigned char)(arg & 0xff);
    data->set.proxy_ssl.enable_beast = !!(arg & CURLSSLOPT_ALLOW_BEAST);
    data->set.proxy_ssl.no_revoke = !!(arg & CURLSSLOPT_NO_REVOKE);
    data->set.proxy_ssl.no_partialchain = !!(arg & CURLSSLOPT_NO_PARTIALCHAIN);
    data->set.proxy_ssl.revoke_best_effort =
      !!(arg & CURLSSLOPT_REVOKE_BEST_EFFORT);
    data->set.proxy_ssl.native_ca_store = !!(arg & CURLSSLOPT_NATIVE_CA);
    data->set.proxy_ssl.auto_client_cert =
      !!(arg & CURLSSLOPT_AUTO_CLIENT_CERT);
    break;
#endif

  case CURLOPT_SSL_EC_CURVES:
    /*
     * Set accepted curves in SSL connection setup.
     * Specify colon-delimited list of curve algorithm names.
     */
    result = Curl_setstropt(&data->set.str[STRING_SSL_EC_CURVES],
                            va_arg(param, char *));
    break;
#endif
  case CURLOPT_IPRESOLVE:
    arg = va_arg(param, long);
    if((arg < CURL_IPRESOLVE_WHATEVER) || (arg > CURL_IPRESOLVE_V6))
      return CURLE_BAD_FUNCTION_ARGUMENT;
    data->set.ipver = (unsigned char) arg;
    break;

  case CURLOPT_MAXFILESIZE_LARGE:
    /*
     * Set the maximum size of a file to download.
     */
    bigsize = va_arg(param, curl_off_t);
    if(bigsize < 0)
      return CURLE_BAD_FUNCTION_ARGUMENT;
    data->set.max_filesize = bigsize;
    break;

  case CURLOPT_TCP_NODELAY:
    /*
     * Enable or disable TCP_NODELAY, which will disable/enable the Nagle
     * algorithm
     */
    data->set.tcp_nodelay = (0 != va_arg(param, long));
    break;

  case CURLOPT_IGNORE_CONTENT_LENGTH:
    data->set.ignorecl = (0 != va_arg(param, long));
    break;

  case CURLOPT_CONNECT_ONLY:
    /*
     * No data transfer.
     * (1) - only do connection
     * (2) - do first get request but get no content
     */
    arg = va_arg(param, long);
    if(arg > 2)
      return CURLE_BAD_FUNCTION_ARGUMENT;
    data->set.connect_only = (unsigned char)arg;
    break;

  case CURLOPT_SOCKOPTFUNCTION:
    /*
     * socket callback function: called after socket() but before connect()
     */
    data->set.fsockopt = va_arg(param, curl_sockopt_callback);
    break;

  case CURLOPT_SOCKOPTDATA:
    /*
     * socket callback data pointer. Might be NULL.
     */
    data->set.sockopt_client = va_arg(param, void *);
    break;

  case CURLOPT_OPENSOCKETFUNCTION:
    /*
     * open/create socket callback function: called instead of socket(),
     * before connect()
     */
    data->set.fopensocket = va_arg(param, curl_opensocket_callback);
    break;

  case CURLOPT_OPENSOCKETDATA:
    /*
     * socket callback data pointer. Might be NULL.
     */
    data->set.opensocket_client = va_arg(param, void *);
    break;

  case CURLOPT_CLOSESOCKETFUNCTION:
    /*
     * close socket callback function: called instead of close()
     * when shutting down a connection
     */
    data->set.fclosesocket = va_arg(param, curl_closesocket_callback);
    break;

  case CURLOPT_RESOLVER_START_FUNCTION:
    /*
     * resolver start callback function: called before a new resolver request
     * is started
     */
    data->set.resolver_start = va_arg(param, curl_resolver_start_callback);
    break;

  case CURLOPT_RESOLVER_START_DATA:
    /*
     * resolver start callback data pointer. Might be NULL.
     */
    data->set.resolver_start_client = va_arg(param, void *);
    break;

  case CURLOPT_CLOSESOCKETDATA:
    /*
     * socket callback data pointer. Might be NULL.
     */
    data->set.closesocket_client = va_arg(param, void *);
    break;

  case CURLOPT_SSL_SESSIONID_CACHE:
    data->set.ssl.primary.sessionid = (0 != va_arg(param, long));
#ifndef CURL_DISABLE_PROXY
    data->set.proxy_ssl.primary.sessionid = data->set.ssl.primary.sessionid;
#endif
    break;

#ifdef USE_SSH
    /* we only include SSH options if explicitly built to support SSH */
  case CURLOPT_SSH_AUTH_TYPES:
    data->set.ssh_auth_types = (unsigned int)va_arg(param, long);
    break;

  case CURLOPT_SSH_PUBLIC_KEYFILE:
    /*
     * Use this file instead of the $HOME/.ssh/id_dsa.pub file
     */
    result = Curl_setstropt(&data->set.str[STRING_SSH_PUBLIC_KEY],
                            va_arg(param, char *));
    break;

  case CURLOPT_SSH_PRIVATE_KEYFILE:
    /*
     * Use this file instead of the $HOME/.ssh/id_dsa file
     */
    result = Curl_setstropt(&data->set.str[STRING_SSH_PRIVATE_KEY],
                            va_arg(param, char *));
    break;
  case CURLOPT_SSH_HOST_PUBLIC_KEY_MD5:
    /*
     * Option to allow for the MD5 of the host public key to be checked
     * for validation purposes.
     */
    result = Curl_setstropt(&data->set.str[STRING_SSH_HOST_PUBLIC_KEY_MD5],
                            va_arg(param, char *));
    break;

  case CURLOPT_SSH_KNOWNHOSTS:
    /*
     * Store the file name to read known hosts from.
     */
    result = Curl_setstropt(&data->set.str[STRING_SSH_KNOWNHOSTS],
                            va_arg(param, char *));
    break;
#ifdef USE_LIBSSH2
  case CURLOPT_SSH_HOST_PUBLIC_KEY_SHA256:
    /*
     * Option to allow for the SHA256 of the host public key to be checked
     * for validation purposes.
     */
    result = Curl_setstropt(&data->set.str[STRING_SSH_HOST_PUBLIC_KEY_SHA256],
                            va_arg(param, char *));
    break;

  case CURLOPT_SSH_HOSTKEYFUNCTION:
    /* the callback to check the hostkey without the knownhost file */
    data->set.ssh_hostkeyfunc = va_arg(param, curl_sshhostkeycallback);
    break;

  case CURLOPT_SSH_HOSTKEYDATA:
    /*
     * Custom client data to pass to the SSH keyfunc callback
     */
    data->set.ssh_hostkeyfunc_userp = va_arg(param, void *);
    break;
#endif

  case CURLOPT_SSH_KEYFUNCTION:
    /* setting to NULL is fine since the ssh.c functions themselves will
       then revert to use the internal default */
    data->set.ssh_keyfunc = va_arg(param, curl_sshkeycallback);
    break;

  case CURLOPT_SSH_KEYDATA:
    /*
     * Custom client data to pass to the SSH keyfunc callback
     */
    data->set.ssh_keyfunc_userp = va_arg(param, void *);
    break;

  case CURLOPT_SSH_COMPRESSION:
    data->set.ssh_compression = (0 != va_arg(param, long))?TRUE:FALSE;
    break;
#endif /* USE_SSH */

  case CURLOPT_HTTP_TRANSFER_DECODING:
    /*
     * disable libcurl transfer encoding is used
     */
#ifndef USE_HYPER
    data->set.http_te_skip = (0 == va_arg(param, long));
    break;
#else
    return CURLE_NOT_BUILT_IN; /* hyper doesn't support */
#endif

  case CURLOPT_HTTP_CONTENT_DECODING:
    /*
     * raw data passed to the application when content encoding is used
     */
    data->set.http_ce_skip = (0 == va_arg(param, long));
    break;

#if !defined(CURL_DISABLE_FTP) || defined(USE_SSH)
  case CURLOPT_NEW_FILE_PERMS:
    /*
     * Uses these permissions instead of 0644
     */
    arg = va_arg(param, long);
    if((arg < 0) || (arg > 0777))
      return CURLE_BAD_FUNCTION_ARGUMENT;
    data->set.new_file_perms = (unsigned int)arg;
    break;
#endif
#ifdef USE_SSH
  case CURLOPT_NEW_DIRECTORY_PERMS:
    /*
     * Uses these permissions instead of 0755
     */
    arg = va_arg(param, long);
    if((arg < 0) || (arg > 0777))
      return CURLE_BAD_FUNCTION_ARGUMENT;
    data->set.new_directory_perms = (unsigned int)arg;
    break;
#endif

#ifdef USE_IPV6
  case CURLOPT_ADDRESS_SCOPE:
    /*
     * Use this scope id when using IPv6
     * We always get longs when passed plain numericals so we should check
     * that the value fits into an unsigned 32 bit integer.
     */
    uarg = va_arg(param, unsigned long);
#if SIZEOF_LONG > 4
    if(uarg > UINT_MAX)
      return CURLE_BAD_FUNCTION_ARGUMENT;
#endif
    data->set.scope_id = (unsigned int)uarg;
    break;
#endif

  case CURLOPT_PROTOCOLS:
    /* set the bitmask for the protocols that are allowed to be used for the
       transfer, which thus helps the app which takes URLs from users or other
       external inputs and want to restrict what protocol(s) to deal
       with. Defaults to CURLPROTO_ALL. */
    data->set.allowed_protocols = (curl_prot_t)va_arg(param, long);
    break;

  case CURLOPT_REDIR_PROTOCOLS:
    /* set the bitmask for the protocols that libcurl is allowed to follow to,
       as a subset of the CURLOPT_PROTOCOLS ones. That means the protocol needs
       to be set in both bitmasks to be allowed to get redirected to. */
    data->set.redir_protocols = (curl_prot_t)va_arg(param, long);
    break;

  case CURLOPT_PROTOCOLS_STR: {
    argptr = va_arg(param, char *);
    result = protocol2num(argptr, &data->set.allowed_protocols);
    if(result)
      return result;
    break;
  }

  case CURLOPT_REDIR_PROTOCOLS_STR: {
    argptr = va_arg(param, char *);
    result = protocol2num(argptr, &data->set.redir_protocols);
    if(result)
      return result;
    break;
  }

  case CURLOPT_DEFAULT_PROTOCOL:
    /* Set the protocol to use when the URL doesn't include any protocol */
    result = Curl_setstropt(&data->set.str[STRING_DEFAULT_PROTOCOL],
                            va_arg(param, char *));
    break;
#ifndef CURL_DISABLE_SMTP
  case CURLOPT_MAIL_FROM:
    /* Set the SMTP mail originator */
    result = Curl_setstropt(&data->set.str[STRING_MAIL_FROM],
                            va_arg(param, char *));
    break;

  case CURLOPT_MAIL_AUTH:
    /* Set the SMTP auth originator */
    result = Curl_setstropt(&data->set.str[STRING_MAIL_AUTH],
                            va_arg(param, char *));
    break;

  case CURLOPT_MAIL_RCPT:
    /* Set the list of mail recipients */
    data->set.mail_rcpt = va_arg(param, struct curl_slist *);
    break;
  case CURLOPT_MAIL_RCPT_ALLOWFAILS:
    /* allow RCPT TO command to fail for some recipients */
    data->set.mail_rcpt_allowfails = (0 != va_arg(param, long));
    break;
#endif

  case CURLOPT_SASL_AUTHZID:
    /* Authorization identity (identity to act as) */
    result = Curl_setstropt(&data->set.str[STRING_SASL_AUTHZID],
                            va_arg(param, char *));
    break;

  case CURLOPT_SASL_IR:
    /* Enable/disable SASL initial response */
    data->set.sasl_ir = (0 != va_arg(param, long));
    break;
#ifndef CURL_DISABLE_RTSP
  case CURLOPT_RTSP_REQUEST:
  {
    /*
     * Set the RTSP request method (OPTIONS, SETUP, PLAY, etc...)
     * Would this be better if the RTSPREQ_* were just moved into here?
     */
    long in_rtspreq = va_arg(param, long);
    Curl_RtspReq rtspreq = RTSPREQ_NONE;
    switch(in_rtspreq) {
    case CURL_RTSPREQ_OPTIONS:
      rtspreq = RTSPREQ_OPTIONS;
      break;

    case CURL_RTSPREQ_DESCRIBE:
      rtspreq = RTSPREQ_DESCRIBE;
      break;

    case CURL_RTSPREQ_ANNOUNCE:
      rtspreq = RTSPREQ_ANNOUNCE;
      break;

    case CURL_RTSPREQ_SETUP:
      rtspreq = RTSPREQ_SETUP;
      break;

    case CURL_RTSPREQ_PLAY:
      rtspreq = RTSPREQ_PLAY;
      break;

    case CURL_RTSPREQ_PAUSE:
      rtspreq = RTSPREQ_PAUSE;
      break;

    case CURL_RTSPREQ_TEARDOWN:
      rtspreq = RTSPREQ_TEARDOWN;
      break;

    case CURL_RTSPREQ_GET_PARAMETER:
      rtspreq = RTSPREQ_GET_PARAMETER;
      break;

    case CURL_RTSPREQ_SET_PARAMETER:
      rtspreq = RTSPREQ_SET_PARAMETER;
      break;

    case CURL_RTSPREQ_RECORD:
      rtspreq = RTSPREQ_RECORD;
      break;

    case CURL_RTSPREQ_RECEIVE:
      rtspreq = RTSPREQ_RECEIVE;
      break;
    default:
      rtspreq = RTSPREQ_NONE;
    }

    data->set.rtspreq = rtspreq;
    break;
  }


  case CURLOPT_RTSP_SESSION_ID:
    /*
     * Set the RTSP Session ID manually. Useful if the application is
     * resuming a previously established RTSP session
     */
    result = Curl_setstropt(&data->set.str[STRING_RTSP_SESSION_ID],
                            va_arg(param, char *));
    break;

  case CURLOPT_RTSP_STREAM_URI:
    /*
     * Set the Stream URI for the RTSP request. Unless the request is
     * for generic server options, the application will need to set this.
     */
    result = Curl_setstropt(&data->set.str[STRING_RTSP_STREAM_URI],
                            va_arg(param, char *));
    break;

  case CURLOPT_RTSP_TRANSPORT:
    /*
     * The content of the Transport: header for the RTSP request
     */
    result = Curl_setstropt(&data->set.str[STRING_RTSP_TRANSPORT],
                            va_arg(param, char *));
    break;

  case CURLOPT_RTSP_CLIENT_CSEQ:
    /*
     * Set the CSEQ number to issue for the next RTSP request. Useful if the
     * application is resuming a previously broken connection. The CSEQ
     * will increment from this new number henceforth.
     */
    data->state.rtsp_next_client_CSeq = va_arg(param, long);
    break;

  case CURLOPT_RTSP_SERVER_CSEQ:
    /* Same as the above, but for server-initiated requests */
    data->state.rtsp_next_server_CSeq = va_arg(param, long);
    break;

  case CURLOPT_INTERLEAVEDATA:
    data->set.rtp_out = va_arg(param, void *);
    break;
  case CURLOPT_INTERLEAVEFUNCTION:
    /* Set the user defined RTP write function */
    data->set.fwrite_rtp = va_arg(param, curl_write_callback);
    break;
#endif
#ifndef CURL_DISABLE_FTP
  case CURLOPT_WILDCARDMATCH:
    data->set.wildcard_enabled = (0 != va_arg(param, long));
    break;
  case CURLOPT_CHUNK_BGN_FUNCTION:
    data->set.chunk_bgn = va_arg(param, curl_chunk_bgn_callback);
    break;
  case CURLOPT_CHUNK_END_FUNCTION:
    data->set.chunk_end = va_arg(param, curl_chunk_end_callback);
    break;
  case CURLOPT_FNMATCH_FUNCTION:
    data->set.fnmatch = va_arg(param, curl_fnmatch_callback);
    break;
  case CURLOPT_CHUNK_DATA:
    data->set.wildcardptr = va_arg(param, void *);
    break;
  case CURLOPT_FNMATCH_DATA:
    data->set.fnmatch_data = va_arg(param, void *);
    break;
#endif
#ifdef USE_TLS_SRP
  case CURLOPT_TLSAUTH_USERNAME:
    result = Curl_setstropt(&data->set.str[STRING_TLSAUTH_USERNAME],
                            va_arg(param, char *));
    break;
#ifndef CURL_DISABLE_PROXY
  case CURLOPT_PROXY_TLSAUTH_USERNAME:
    result = Curl_setstropt(&data->set.str[STRING_TLSAUTH_USERNAME_PROXY],
                            va_arg(param, char *));
    break;
#endif
  case CURLOPT_TLSAUTH_PASSWORD:
    result = Curl_setstropt(&data->set.str[STRING_TLSAUTH_PASSWORD],
                            va_arg(param, char *));
    break;
#ifndef CURL_DISABLE_PROXY
  case CURLOPT_PROXY_TLSAUTH_PASSWORD:
    result = Curl_setstropt(&data->set.str[STRING_TLSAUTH_PASSWORD_PROXY],
                            va_arg(param, char *));
    break;
#endif
  case CURLOPT_TLSAUTH_TYPE:
    argptr = va_arg(param, char *);
    if(argptr && !strcasecompare(argptr, "SRP"))
      return CURLE_BAD_FUNCTION_ARGUMENT;
    break;
#ifndef CURL_DISABLE_PROXY
  case CURLOPT_PROXY_TLSAUTH_TYPE:
    argptr = va_arg(param, char *);
    if(argptr && !strcasecompare(argptr, "SRP"))
      return CURLE_BAD_FUNCTION_ARGUMENT;
    break;
#endif
#endif
#ifdef USE_ARES
  case CURLOPT_DNS_SERVERS:
    result = Curl_setstropt(&data->set.str[STRING_DNS_SERVERS],
                            va_arg(param, char *));
    if(result)
      return result;
    result = Curl_set_dns_servers(data, data->set.str[STRING_DNS_SERVERS]);
    break;
  case CURLOPT_DNS_INTERFACE:
    result = Curl_setstropt(&data->set.str[STRING_DNS_INTERFACE],
                            va_arg(param, char *));
    if(result)
      return result;
    result = Curl_set_dns_interface(data, data->set.str[STRING_DNS_INTERFACE]);
    break;
  case CURLOPT_DNS_LOCAL_IP4:
    result = Curl_setstropt(&data->set.str[STRING_DNS_LOCAL_IP4],
                            va_arg(param, char *));
    if(result)
      return result;
    result = Curl_set_dns_local_ip4(data, data->set.str[STRING_DNS_LOCAL_IP4]);
    break;
  case CURLOPT_DNS_LOCAL_IP6:
    result = Curl_setstropt(&data->set.str[STRING_DNS_LOCAL_IP6],
                            va_arg(param, char *));
    if(result)
      return result;
    result = Curl_set_dns_local_ip6(data, data->set.str[STRING_DNS_LOCAL_IP6]);
    break;
#endif
  case CURLOPT_TCP_KEEPALIVE:
    data->set.tcp_keepalive = (0 != va_arg(param, long));
    break;
  case CURLOPT_TCP_KEEPIDLE:
    arg = va_arg(param, long);
    if(arg < 0)
      return CURLE_BAD_FUNCTION_ARGUMENT;
    else if(arg > INT_MAX)
      arg = INT_MAX;
    data->set.tcp_keepidle = (int)arg;
    break;
  case CURLOPT_TCP_KEEPINTVL:
    arg = va_arg(param, long);
    if(arg < 0)
      return CURLE_BAD_FUNCTION_ARGUMENT;
    else if(arg > INT_MAX)
      arg = INT_MAX;
    data->set.tcp_keepintvl = (int)arg;
    break;
  case CURLOPT_TCP_FASTOPEN:
#if defined(CONNECT_DATA_IDEMPOTENT) || defined(MSG_FASTOPEN) || \
   defined(TCP_FASTOPEN_CONNECT)
    data->set.tcp_fastopen = (0 != va_arg(param, long))?TRUE:FALSE;
#else
    result = CURLE_NOT_BUILT_IN;
#endif
    break;
  case CURLOPT_SSL_ENABLE_NPN:
    break;
  case CURLOPT_SSL_ENABLE_ALPN:
    data->set.ssl_enable_alpn = (0 != va_arg(param, long));
    break;
#ifdef USE_UNIX_SOCKETS
  case CURLOPT_UNIX_SOCKET_PATH:
    data->set.abstract_unix_socket = FALSE;
    result = Curl_setstropt(&data->set.str[STRING_UNIX_SOCKET_PATH],
                            va_arg(param, char *));
    break;
  case CURLOPT_ABSTRACT_UNIX_SOCKET:
    data->set.abstract_unix_socket = TRUE;
    result = Curl_setstropt(&data->set.str[STRING_UNIX_SOCKET_PATH],
                            va_arg(param, char *));
    break;
#endif

  case CURLOPT_PATH_AS_IS:
    data->set.path_as_is = (0 != va_arg(param, long));
    break;
  case CURLOPT_PIPEWAIT:
    data->set.pipewait = (0 != va_arg(param, long));
    break;
  case CURLOPT_STREAM_WEIGHT:
#if defined(USE_HTTP2) || defined(USE_HTTP3)
    arg = va_arg(param, long);
    if((arg >= 1) && (arg <= 256))
      data->set.priority.weight = (int)arg;
    break;
#else
    return CURLE_NOT_BUILT_IN;
#endif
  case CURLOPT_STREAM_DEPENDS:
  case CURLOPT_STREAM_DEPENDS_E:
  {
    struct Curl_easy *dep = va_arg(param, struct Curl_easy *);
    if(!dep || GOOD_EASY_HANDLE(dep)) {
      return Curl_data_priority_add_child(dep, data,
                                          option == CURLOPT_STREAM_DEPENDS_E);
    }
    break;
  }
  case CURLOPT_CONNECT_TO:
    data->set.connect_to = va_arg(param, struct curl_slist *);
    break;
  case CURLOPT_SUPPRESS_CONNECT_HEADERS:
    data->set.suppress_connect_headers = (0 != va_arg(param, long))?TRUE:FALSE;
    break;
  case CURLOPT_HAPPY_EYEBALLS_TIMEOUT_MS:
    uarg = va_arg(param, unsigned long);
    if(uarg > UINT_MAX)
      uarg = UINT_MAX;
    data->set.happy_eyeballs_timeout = (unsigned int)uarg;
    break;
#ifndef CURL_DISABLE_SHUFFLE_DNS
  case CURLOPT_DNS_SHUFFLE_ADDRESSES:
    data->set.dns_shuffle_addresses = (0 != va_arg(param, long));
    break;
#endif
  case CURLOPT_DISALLOW_USERNAME_IN_URL:
    data->set.disallow_username_in_url = (0 != va_arg(param, long));
    break;
#ifndef CURL_DISABLE_DOH
  case CURLOPT_DOH_URL:
    result = Curl_setstropt(&data->set.str[STRING_DOH],
                            va_arg(param, char *));
    data->set.doh = data->set.str[STRING_DOH]?TRUE:FALSE;
    break;
#endif
  case CURLOPT_UPKEEP_INTERVAL_MS:
    arg = va_arg(param, long);
    if(arg < 0)
      return CURLE_BAD_FUNCTION_ARGUMENT;
    data->set.upkeep_interval_ms = arg;
    break;
  case CURLOPT_MAXAGE_CONN:
    arg = va_arg(param, long);
    if(arg < 0)
      return CURLE_BAD_FUNCTION_ARGUMENT;
    data->set.maxage_conn = arg;
    break;
  case CURLOPT_MAXLIFETIME_CONN:
    arg = va_arg(param, long);
    if(arg < 0)
      return CURLE_BAD_FUNCTION_ARGUMENT;
    data->set.maxlifetime_conn = arg;
    break;
  case CURLOPT_TRAILERFUNCTION:
#ifndef CURL_DISABLE_HTTP
    data->set.trailer_callback = va_arg(param, curl_trailer_callback);
#endif
    break;
  case CURLOPT_TRAILERDATA:
#ifndef CURL_DISABLE_HTTP
    data->set.trailer_data = va_arg(param, void *);
#endif
    break;
#ifndef CURL_DISABLE_HSTS
  case CURLOPT_HSTSREADFUNCTION:
    data->set.hsts_read = va_arg(param, curl_hstsread_callback);
    break;
  case CURLOPT_HSTSREADDATA:
    data->set.hsts_read_userp = va_arg(param, void *);
    break;
  case CURLOPT_HSTSWRITEFUNCTION:
    data->set.hsts_write = va_arg(param, curl_hstswrite_callback);
    break;
  case CURLOPT_HSTSWRITEDATA:
    data->set.hsts_write_userp = va_arg(param, void *);
    break;
  case CURLOPT_HSTS: {
    struct curl_slist *h;
    if(!data->hsts) {
      data->hsts = Curl_hsts_init();
      if(!data->hsts)
        return CURLE_OUT_OF_MEMORY;
    }
    argptr = va_arg(param, char *);
    if(argptr) {
      result = Curl_setstropt(&data->set.str[STRING_HSTS], argptr);
      if(result)
        return result;
      /* this needs to build a list of file names to read from, so that it can
         read them later, as we might get a shared HSTS handle to load them
         into */
      h = curl_slist_append(data->state.hstslist, argptr);
      if(!h) {
        curl_slist_free_all(data->state.hstslist);
        data->state.hstslist = NULL;
        return CURLE_OUT_OF_MEMORY;
      }
      data->state.hstslist = h; /* store the list for later use */
    }
    else {
      /* clear the list of HSTS files */
      curl_slist_free_all(data->state.hstslist);
      data->state.hstslist = NULL;
      if(!data->share || !data->share->hsts)
        /* throw away the HSTS cache unless shared */
        Curl_hsts_cleanup(&data->hsts);
    }
    break;
  }
  case CURLOPT_HSTS_CTRL:
    arg = va_arg(param, long);
    if(arg & CURLHSTS_ENABLE) {
      if(!data->hsts) {
        data->hsts = Curl_hsts_init();
        if(!data->hsts)
          return CURLE_OUT_OF_MEMORY;
      }
    }
    else
      Curl_hsts_cleanup(&data->hsts);
    break;
#endif
#ifndef CURL_DISABLE_ALTSVC
  case CURLOPT_ALTSVC:
    if(!data->asi) {
      data->asi = Curl_altsvc_init();
      if(!data->asi)
        return CURLE_OUT_OF_MEMORY;
    }
    argptr = va_arg(param, char *);
    result = Curl_setstropt(&data->set.str[STRING_ALTSVC], argptr);
    if(result)
      return result;
    if(argptr)
      (void)Curl_altsvc_load(data->asi, argptr);
    break;
  case CURLOPT_ALTSVC_CTRL:
    if(!data->asi) {
      data->asi = Curl_altsvc_init();
      if(!data->asi)
        return CURLE_OUT_OF_MEMORY;
    }
    arg = va_arg(param, long);
    if(!arg) {
      DEBUGF(infof(data, "bad CURLOPT_ALTSVC_CTRL input"));
      return CURLE_BAD_FUNCTION_ARGUMENT;
    }
    result = Curl_altsvc_ctrl(data->asi, arg);
    if(result)
      return result;
    break;
#endif
  case CURLOPT_PREREQFUNCTION:
    data->set.fprereq = va_arg(param, curl_prereq_callback);
    break;
  case CURLOPT_PREREQDATA:
    data->set.prereq_userp = va_arg(param, void *);
    break;
#ifdef USE_WEBSOCKETS
  case CURLOPT_WS_OPTIONS: {
    bool raw;
    arg = va_arg(param, long);
    raw = (arg & CURLWS_RAW_MODE);
    data->set.ws_raw_mode = raw;
    break;
  }
#endif
#ifdef USE_ECH
  case CURLOPT_ECH: {
    size_t plen = 0;

    argptr = va_arg(param, char *);
    if(!argptr) {
      data->set.tls_ech = CURLECH_DISABLE;
      result = CURLE_BAD_FUNCTION_ARGUMENT;
      return result;
    }
    plen = strlen(argptr);
    if(plen > CURL_MAX_INPUT_LENGTH) {
      data->set.tls_ech = CURLECH_DISABLE;
      result = CURLE_BAD_FUNCTION_ARGUMENT;
      return result;
    }
    /* set tls_ech flag value, preserving CLA_CFG bit */
    if(plen == 5 && !strcmp(argptr, "false"))
      data->set.tls_ech = CURLECH_DISABLE
                          | (data->set.tls_ech & CURLECH_CLA_CFG);
    else if(plen == 6 && !strcmp(argptr, "grease"))
      data->set.tls_ech = CURLECH_GREASE
                          | (data->set.tls_ech & CURLECH_CLA_CFG);
    else if(plen == 4 && !strcmp(argptr, "true"))
      data->set.tls_ech = CURLECH_ENABLE
                          | (data->set.tls_ech & CURLECH_CLA_CFG);
    else if(plen == 4 && !strcmp(argptr, "hard"))
      data->set.tls_ech = CURLECH_HARD
                          | (data->set.tls_ech & CURLECH_CLA_CFG);
    else if(plen > 5 && !strncmp(argptr, "ecl:", 4)) {
      result = Curl_setstropt(&data->set.str[STRING_ECH_CONFIG], argptr + 4);
      if(result)
        return result;
      data->set.tls_ech |= CURLECH_CLA_CFG;
    }
    else if(plen > 4 && !strncmp(argptr, "pn:", 3)) {
      result = Curl_setstropt(&data->set.str[STRING_ECH_PUBLIC], argptr + 3);
      if(result)
        return result;
    }
    break;
  }
#endif
  case CURLOPT_QUICK_EXIT:
    data->set.quick_exit = (0 != va_arg(param, long)) ? 1L:0L;
    break;
#ifdef HTTP_HANDOVER_FEATURE
  case CURLOPT_OHOS_SOCKET_BIND_NET_ID:
    uarg = va_arg(param, unsigned long);
#define MAX_NET_ID (0xFFFF - 0x400)
    if (uarg > MAX_NET_ID)
      return CURLE_BAD_FUNCTION_ARGUMENT;
    data->set.socket_bind_netid = uarg;
    break;
  case CURLOPT_CONNREUSEFUNCTION:
    data->set.fconnreuse = va_arg(param, curl_connreuse_callback);
    break;
  case CURLOPT_CONNREUSEDATA:
    data->set.connreuse_userp = va_arg(param, void *);
    break;
#endif
  case CURLOPT_MMS_RESERVED_DEFAULT_PORT:
    data->set.mms_reserved_default_port = (0 != va_arg(param, long)) ? 1L:0L;
    break;
  default:
    /* unknown tag and its companion, just ignore: */
    result = CURLE_UNKNOWN_OPTION;
    break;
  }

  return result;
}

/*
 * curl_easy_setopt() is the external interface for setting options on an
 * easy handle.
 *
 * NOTE: This is one of few API functions that are allowed to be called from
 * within a callback.
 */

#undef curl_easy_setopt
CURLcode curl_easy_setopt(struct Curl_easy *data, CURLoption tag, ...)
{
  va_list arg;
  CURLcode result;

  if(!data)
    return CURLE_BAD_FUNCTION_ARGUMENT;

  va_start(arg, tag);

  result = Curl_vsetopt(data, tag, arg);

  va_end(arg);
#ifdef DEBUGBUILD
  if(result == CURLE_BAD_FUNCTION_ARGUMENT)
    infof(data, "setopt arg 0x%x returned CURLE_BAD_FUNCTION_ARGUMENT", tag);
#endif
  return result;
}
