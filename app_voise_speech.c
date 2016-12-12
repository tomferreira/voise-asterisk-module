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

#include <voise_client.h>

//#define TRACE_ENABLED

#define AST_4   10400
#define AST_6   10600
#define AST_601 10601
#define AST_8   10800
#define AST_10  100000
#define AST_11  110000
#define AST_13  130000

#ifdef TRACE_ENABLED
#define TRACE_FUNCTION() \
    ast_log(LOG_DEBUG, "%s\n", __FUNCTION__)
#else
    #define TRACE_FUNCTION()
#endif

#if ASTERISK_VERSION_NUM < AST_8
    typedef void* DATA_TYPE;
#else
    typedef const char* DATA_TYPE;  
#endif

#if ASTERISK_VERSION_NUM == AST_13
    #include "asterisk/format_cache.h"
    #define AUDIO_FORMAT    ast_format_alaw
#else
    #define AUDIO_FORMAT    AST_FORMAT_ALAW
#endif

static const char *VOISE_CFG = "voise.conf";
static const char *VOISE_DEF_HOST = "127.0.0.1";
static const char *VOISE_DEF_LANG = "pt-BR";
static const char *VOISE_DEF_VERBOSE = "0"; /* disabled */

static const int  VOISE_BUFFER_SIZE  = 160; /* Alaw 20ms */
static const char *VOISE_ENCODING = "ALAW";
static const int  MAX_WAIT_TIME = 1000; /*ms*/

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

    #if ASTERISK_VERSION_NUM < AST_6 
        return ast_config_load(VOISE_CFG); 
    #else
        struct ast_flags config_flags = { CONFIG_FLAG_WITHCOMMENTS };
        return ast_config_load(VOISE_CFG, config_flags);
    #endif 
}

/*! \brief Helper function. Set channel write format*/
static int voise_set_channel_write_format(struct ast_channel *chan)
{
    TRACE_FUNCTION();

    #if (ASTERISK_VERSION_NUM < AST_10) || (ASTERISK_VERSION_NUM == AST_13)
        return ast_set_write_format(chan, AUDIO_FORMAT);
    #else
        return ast_set_write_format_by_id(chan, AUDIO_FORMAT);
    #endif
}

/*! \brief Helper function. Set channel read format*/
static int voise_set_channel_read_format(struct ast_channel *chan)
{
    TRACE_FUNCTION();

    #if (ASTERISK_VERSION_NUM < AST_10) || (ASTERISK_VERSION_NUM == AST_13)
        return ast_set_read_format(chan, AUDIO_FORMAT);
    #else
        return ast_set_read_format_by_id(chan, AUDIO_FORMAT);
    #endif
}

static const char* voise_get_chan_name(struct ast_channel *chan)
{
    TRACE_FUNCTION();

    #if ASTERISK_VERSION_NUM < AST_11
        return chan->name;
    #else
        return ast_channel_name(chan);
    #endif
}

static const char* voise_get_chan_language(struct ast_channel *chan)
{
    TRACE_FUNCTION();

    #if ASTERISK_VERSION_NUM < AST_11
        return chan->language;
    #else
        return ast_channel_language(chan);
    #endif
}

#if ASTERISK_VERSION_NUM == AST_13
static int voise_get_bytes_per_sample(struct ast_format *format)
{
    if (format == ast_format_ulaw || format == ast_format_alaw)
        return 1;

    /* linear */
    return 2;
}
#else
static int voise_get_bytes_per_sample(int formatid)
{
    if (formatid == AST_FORMAT_ULAW || formatid == AST_FORMAT_ALAW)
        return 1;

    /* linear */
    return 2;
}
#endif

/*! \brief Text to speech application. */
static int voise_say_exec(struct ast_channel *chan, DATA_TYPE data)
{
    TRACE_FUNCTION();

    #if ASTERISK_VERSION_NUM < AST_4
    struct localuser *u;
    #else
    struct ast_module_user *u;
    #endif

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

    #if ASTERISK_VERSION_NUM < AST_4
    LOCAL_USER_ADD(u);
    #else
    u = ast_module_user_add(chan);
    #endif

    /* Set channel format */
    if (voise_set_channel_write_format(chan) < 0)
    {
        ast_log(LOG_ERROR, "AUDIO_FORMAT (write) failed.\n");

        #if ASTERISK_VERSION_NUM < AST_4
        LOCAL_USER_REMOVE(u);
        #else   
        ast_module_user_remove(u);
        #endif

        ast_config_destroy(vcfg);

        return -1;
    }

    if (voise_set_channel_read_format(chan) < 0)
    {
        ast_log(LOG_ERROR, "AUDIO_FORMAT (read) failed.\n");

        #if ASTERISK_VERSION_NUM < AST_4
        LOCAL_USER_REMOVE(u);
        #else   
        ast_module_user_remove(u);
        #endif

        ast_config_destroy(vcfg);

        return -1;
    }

    voise_client_t *client = voise_init(vserverip, 8100, 1);

    if (client == NULL)
    {
        ast_log(LOG_ERROR, "Could not connect to Voise server (%s).\n", vserverip);

        #if ASTERISK_VERSION_NUM < AST_4
        LOCAL_USER_REMOVE(u);
        #else   
        ast_module_user_remove(u);
        #endif

        ast_config_destroy(vcfg);

        return -1;
    }

    /* Answer if it's not already going. */
    if (ast_channel_state(chan) != AST_STATE_UP)
        ast_answer(chan);

    /* Ensure no streams are currently running.. */
    ast_stopstream(chan);

    voise_response_t response;
    voise_start_synth(client, &response, args.text, VOISE_ENCODING, 8000, args.lang);

    if (response.result_code != 201)
    {
        ast_log(LOG_ERROR, "VoiseSay: %s\n", response.result_message);

        #if ASTERISK_VERSION_NUM < AST_4
        LOCAL_USER_REMOVE(u);
        #else   
        ast_module_user_remove(u);
        #endif

        voise_close(client);

        ast_config_destroy(vcfg);

        return -1;
    }

    ast_config_destroy(vcfg);

    if (option_beep) 
    {
        int res = ast_streamfile(chan, "beep", voise_get_chan_language(chan));

        if (!res) 
            res = ast_waitstream(chan, "");
        else
            ast_log(LOG_WARNING, "ast_streamfile failed on %s\n", voise_get_chan_name(chan));

        ast_stopstream(chan);
    }

    ast_safe_sleep(chan, 300);

    struct ast_frame *f;
    unsigned char samples[VOISE_BUFFER_SIZE];

    int done = 0;
    while (!done)
    {
        int ms = ast_waitfor(chan, MAX_WAIT_TIME);

        if (option_verbose)
            ast_log(LOG_DEBUG, "Waited %d ms\n", ms);

        if (ms < 0)
        {
            ast_log(LOG_ERROR, "Wait failed.\n");

            voise_close(client);

            ast_stopstream(chan);

            #if ASTERISK_VERSION_NUM < AST_4
            LOCAL_USER_REMOVE(u);
            #else   
            ast_module_user_remove(u);
            #endif

            return -1;  
        }

        if (option_verbose)
            ast_log(LOG_DEBUG, "Going to read a new frame\n");

        f = ast_read(chan);

        /* Hangup detection */
        if (!f)
        {
            ast_log(LOG_DEBUG, "Hangup detected.\n");

            voise_close(client);

            ast_stopstream(chan);

            #if ASTERISK_VERSION_NUM < AST_4
            LOCAL_USER_REMOVE(u);
            #else   
            ast_module_user_remove(u);
            #endif

            return -1;
        }

        if (f->frametype == AST_FRAME_VOICE)
        {
            memset(samples, 0, VOISE_BUFFER_SIZE);

            int count = voise_read_synth(client, samples);

            int nbytes = f->samples * voise_get_bytes_per_sample(AUDIO_FORMAT);

            if (count < nbytes)
                done = 1;

            f->datalen = count;
            f->samples = count / voise_get_bytes_per_sample(AUDIO_FORMAT);

            /* Tell the frame which are it's new samples */
            #if ASTERISK_VERSION_NUM < AST_601
            f->data = samples;
            #else
            f->data.ptr = samples;
            #endif

            if (ast_write(chan, f))
                ast_log(LOG_ERROR, "Error writing frame to chan.\n");
        } 

        ast_frfree(f);
    }

    voise_close(client);

    ast_safe_sleep(chan, 20);

    ast_stopstream(chan);

    #if ASTERISK_VERSION_NUM < AST_4
    LOCAL_USER_REMOVE(u);
    #else   
    ast_module_user_remove(u);
    #endif

    return 0;
}

#if ASTERISK_VERSION_NUM < AST_4
int load_module(void)
#else   
static int load_module(void)
#endif
{
    return ast_register_application(voise_say_app, voise_say_exec, "Text to speech application", voise_say_descrip);
}

#if ASTERISK_VERSION_NUM < AST_4
int unload_module(void)
#else
static int unload_module(void)
#endif
{
    #if ASTERISK_VERSION_NUM < AST_4
    STANDARD_HANGUP_LOCALUSERS;
    #elif ASTERISK_VERSION_NUM < AST_6
    ast_module_user_hangup_all();
    #else
    /* */
    #endif

    return ast_unregister_application(voise_say_app);
}


AST_MODULE_INFO(ASTERISK_GPL_KEY, AST_MODFLAG_DEFAULT, "Voise TTS Application",
    .load = load_module,
    .unload = unload_module,
);