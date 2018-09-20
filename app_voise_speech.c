/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Copyright (C) 2016, Voise
 *
 * Voise <cirillor@lbv.org.br>
 *
 * See http://www.asterisk.org for more information about
 * the Asterisk project. Please do not directly contact
 * any of the maintainers of this project for assistance;
 * the project provides a web site, mailing lists and IRC
 * channels for your use.
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License Version 2. See the LICENSE file
 * at the top of the source tree.
 */

/*! \file
 *
 * \brief Voise connector
 *
 * \author Voise <cirillor@lbv.org.br>
 */

#include "asterisk.h"

ASTERISK_FILE_VERSION(__FILE__, "$Revision: 1 $")

#include "asterisk/file.h"
#include "asterisk/logger.h"
#include "asterisk/channel.h"
#include "asterisk/pbx.h"
#include "asterisk/module.h"
#include "asterisk/lock.h"
#include "asterisk/app.h"
#include "asterisk/format_cache.h"

#include <voise_client.h>

//#define TRACE_ENABLED

#ifdef TRACE_ENABLED
#define TRACE_FUNCTION() \
    ast_log(LOG_DEBUG, "%s\n", __FUNCTION__)
#else
    #define TRACE_FUNCTION()
#endif

static const char *VOISE_CFG = "voise.conf";
static const char *VOISE_DEF_HOST = "127.0.0.1";
static const char *VOISE_DEF_LANG = "pt-BR";
static const char *VOISE_DEF_VERBOSE = "0"; /* disabled */

static const int MAX_WAIT_TIME = 1000; /*ms*/

/*
* Application info
*/
/* VoiseSay */
static char *voise_say_descrip =
"VoiseSay(text[,lang][,options])\n"
"Synthetise a text using Voise TTS engine.\n"
"- text        : text to synth\n"
"- lang        : tts language\n"
"- options     : v (verbosity on)\n"
"                b (beep before prompt)\n"
"                n (do not hangup on Voise error)\n"
"\n";
static char *voise_say_app = "VoiseSay";

/*! \brief Helper function. Read config file*/
static struct ast_config* voise_load_asterisk_config(void)
{
    TRACE_FUNCTION();

    struct ast_flags config_flags = { CONFIG_FLAG_WITHCOMMENTS };
    return ast_config_load(VOISE_CFG, config_flags);
}

static void __voise_capture_error_cb(const char* fmt, ...)
{
    char msg[1000];
    va_list va;

    va_start(va, fmt);

    vsnprintf(msg, 1000, fmt, va);
    ast_log(LOG_ERROR, "libvoise.so -> %s", msg);

    va_end(va);
}

static struct ast_format* ast_channel_get_speechwriteformat(struct ast_channel *chan)
{
    TRACE_FUNCTION();

    struct ast_format *raw_format = ast_channel_rawreadformat(chan);

    if (raw_format == ast_format_ulaw || raw_format == ast_format_alaw)
        return raw_format;

    int sample_rate = ast_format_get_sample_rate(raw_format);

    return ast_format_cache_get_slin_by_rate(sample_rate);
}

static int voise_get_bytes_per_sample(struct ast_format *format)
{
    if (format == ast_format_ulaw || format == ast_format_alaw)
        return 1;

    /* linear */
    return 2;
}

/*! \brief Text to speech application. */
static int voise_say_exec(struct ast_channel *chan, const char* data)
{
    TRACE_FUNCTION();

    struct ast_module_user *u;
    char *parse;

    int option_verbose = 0;
    int option_beep = 0;
    int option_no_hangup_on_err = 0;

    AST_DECLARE_APP_ARGS(args,
        AST_APP_ARG(text);
        AST_APP_ARG(lang);
        AST_APP_ARG(options);
    );

    if (ast_strlen_zero(data))
    {
        ast_log(LOG_ERROR, "%s requires an argument (text[,lang][,options])\n", voise_say_app);
        return -1;
    }

    /* We need to make a copy of the input string if we are going to modify it! */
    parse = ast_strdupa(data);
    AST_STANDARD_APP_ARGS(args, parse);

    if (ast_strlen_zero(args.text))
    {
        ast_log(LOG_WARNING, "%s() requires a text argument (text[,lang][,options])\n", voise_say_app);
        return -1;
    }

    if (!ast_strlen_zero(args.options))
    {
        if (strchr(args.options, 'v'))
            option_verbose = 1;
        if (strchr(args.options, 'b'))
            option_beep = 1;
        if (strchr(args.options, 'n'))
            option_no_hangup_on_err = 1;
    }

    /* Load default options */
    struct ast_config *vcfg = voise_load_asterisk_config();

    if (!vcfg)
    {
        ast_log(LOG_ERROR, "Error opening configuration file %s\n", VOISE_CFG);
        return -1;
    }

    /* If language is not defined, get default lang */
    if (ast_strlen_zero(args.lang))
    {
        if ( !(args.lang = (char*) ast_variable_retrieve(vcfg, "general", "lang")))
            args.lang = (char*) VOISE_DEF_LANG;
    }

    /* If verbosity is not defined, get default verbosity */
    if (!option_verbose)
    {
        const char *vverbose;
        if ( !(vverbose = ast_variable_retrieve(vcfg, "debug", "verbose")))
            vverbose = VOISE_DEF_VERBOSE;

        option_verbose = atoi(vverbose);
    }

    /* Server IP */
    const char *vserverip;
    if ( !(vserverip = ast_variable_retrieve(vcfg, "general", "serverip")) )
        vserverip = VOISE_DEF_HOST;

    u = ast_module_user_add(chan);

    struct ast_format *new_writeformat = ast_channel_get_speechwriteformat(chan);

    int max_frame_ms = ast_format_get_default_ms(new_writeformat);
    // Eg: Alaw: 20 (default_ms) / 10 (get_minimum_ms) * 80 (minimum_bytes) = 160;
    int max_frame_len = max_frame_ms / ast_format_get_minimum_ms(new_writeformat) * ast_format_get_minimum_bytes(new_writeformat);

    if (option_verbose)
        ast_log(LOG_DEBUG, "Format name: %s, Max frame len: %d\n", ast_format_get_name(new_writeformat), max_frame_len);

    /* Set channel format */
    ast_channel_set_writeformat(chan, new_writeformat);

    voise_client_t client;
    int ret = voise_init(&client, vserverip, 8102, 1, __voise_capture_error_cb);

    if (ret < 0)
    {
        ast_log(LOG_ERROR, "Could not connect to Voise server (%s).\n", vserverip);

        ast_module_user_remove(u);
        ast_config_destroy(vcfg);

        return -1;
    }

    /* Answer if it's not already going. */
    if (ast_channel_state(chan) != AST_STATE_UP)
        ast_answer(chan);

    /* Ensure no streams are currently running.. */
    ast_stopstream(chan);

    voise_response_t response;
    ret = voise_start_synth(&client, &response,
        args.text, ast_format_get_name(new_writeformat), ast_format_get_sample_rate(new_writeformat), args.lang, max_frame_ms);

    // 201 = Accepted
    if (ret < 0 || response.result_code != 201)
    {
        ast_log(LOG_ERROR, "VoiseSay: %s\n", response.result_message);

        ast_module_user_remove(u);

        voise_close(&client);

        ast_config_destroy(vcfg);

        return -1;
    }

    ast_config_destroy(vcfg);

    if (option_beep)
    {
        int res = ast_streamfile(chan, "beep", ast_channel_language(chan));

        if (!res)
            res = ast_waitstream(chan, "");
        else
            ast_log(LOG_WARNING, "ast_streamfile failed on %s\n", ast_channel_name(chan));

        ast_stopstream(chan);
    }

    ast_safe_sleep(chan, 300);

    struct ast_frame *f;
    unsigned char audio_data[VOISE_MAX_FRAME_LEN];

    int result = 0;
    int done = 0;

    while (!done)
    {
        int ms = ast_waitfor(chan, MAX_WAIT_TIME);

        if (option_verbose)
            ast_log(LOG_DEBUG, "Waited %d ms\n", ms);

        if (ms < 0)
        {
            ast_log(LOG_ERROR, "Wait failed.\n");

            result = -1;
            break;
        }

        if (option_verbose)
            ast_log(LOG_DEBUG, "Going to read a new frame\n");

        f = ast_read(chan);

        /* Hangup detection */
        if (!f)
        {
            ast_log(LOG_DEBUG, "Hangup detected.\n");

            result = -1;
            break;
        }

        if (f->frametype == AST_FRAME_VOICE)
        {
            memset(audio_data, 0, VOISE_MAX_FRAME_LEN * sizeof(unsigned char));

            size_t audio_len = -1;
            ret = voise_read_synth(&client, audio_data, &audio_len);

            if (ret < 0)
            {
                ast_log(LOG_ERROR, "Read synth error: %d\n", ret);
            }

            int nbytes = f->samples * voise_get_bytes_per_sample(new_writeformat);

            if (audio_len < nbytes)
                done = 1;

            f->datalen = (int)audio_len;
            f->samples = (int)audio_len / voise_get_bytes_per_sample(new_writeformat);
            f->offset = 0;

            /* Tell the frame which are it's new samples */
            f->data.ptr = audio_data;

            if (ast_write(chan, f) < 0)
                ast_log(LOG_ERROR, "Error writing frame to chan.\n");
        }

        ast_frfree(f);
    }

    voise_close(&client);

    ast_safe_sleep(chan, 20);

    ast_stopstream(chan);
    ast_module_user_remove(u);

    return result;
}

static int load_module(void)
{
    return ast_register_application(voise_say_app, voise_say_exec, "Text to speech application", voise_say_descrip);
}

static int unload_module(void)
{
    return ast_unregister_application(voise_say_app);
}


AST_MODULE_INFO(ASTERISK_GPL_KEY, AST_MODFLAG_DEFAULT, "Voise TTS Application",
    .load = load_module,
    .unload = unload_module,
);
