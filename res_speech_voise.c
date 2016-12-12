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

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

#include "asterisk/channel.h"
#include "asterisk/frame.h"
#include "asterisk/dsp.h"
#include "asterisk/module.h"
#include "asterisk/format.h"
#include "asterisk/config.h"
#include "asterisk/speech.h"

#include <voise_client.h>

//#define TRACE_ENABLED

#define AST_4   10400
#define AST_6   10600
#define AST_601 10601
#define AST_8   10800
#define AST_10  100000
#define AST_11  110000
#define AST_13  130000

/* For Asterisk VideoCaps */
#if ASTERISK_VERSION_NUM == 999999 
    #undef ASTERISK_VERSION_NUM
    #define ASTERISK_VERSION_NUM 10499
#endif

#if ASTERISK_VERSION_NUM > AST_10
    #include "asterisk/ast_version.h"
    
    #if ASTERISK_VERSION_NUM >= AST_13
        #include <asterisk/format_cache.h>
    #endif

#else
    #include "asterisk/version.h"
#endif

#ifdef TRACE_ENABLED
#define TRACE_FUNCTION() \
    ast_log(LOG_DEBUG, "%s\n", __FUNCTION__)
#else
    #define TRACE_FUNCTION()
#endif

#define CHECK_NOT_NULL(s, msg, ret) \
    if (s == NULL) { \
        ast_log(LOG_ERROR, "%s\n", msg); \
        return ret; \
    }

static const int  VOISE_BUFSIZE = 2048;
static const int  VOISE_NOISE_FRAMES = 1;
static const int  VOISE_SILENCE_THRESHOLD = 2000;
static const int  VOISE_MAX_NBEST = 1;

static const char *VOISE_CFG = "voise.conf";
static const char *VOISE_DEF_HOST = "127.0.0.1";
static const char *VOISE_DEF_LANG = "pt-BR";
static const char *VOISE_DEF_ASR_ENGINE = "me";
static const char *VOISE_DEF_INIT_SIL = "5000";
static const char *VOISE_DEF_MAX_SIL = "1000";
static const char *VOISE_DEF_ABS_TIMEOUT = "15";
static const char *VOISE_DEF_VERBOSE = "0"; /* disabled */

struct voise_speech_info
{
    /* Client */
    voise_client_t *client;

    /* Verbosity */
    int verbose;

    /* Language code used */
    char lang[10];

    /* ASR engine used */
    char asr_engine[10];

    /* Model (i.e. pseudo grammar) used */
    char model_name[1000];

    /* Maximum duration of initial silence (in milliseconds) */
    int initsil;

    /* Maximum duration of final silence (in milliseconds) */
    int maxsil;

    /* Absolute timeout for recognition (in seconds) */
    int abs_timeout;

    /* True if we have detected speech */
    int heardspeech;

    /* Number of consecutive non-silent frames */
    int noiseframes;

    /* Start time of recognition's stream */
    time_t start_time;

    /* Holds our silence-detection DSP */
    struct ast_dsp *dsp;
};

static struct ast_speech_engine voise_engine;

/* ********************************* */
/* ************ Helpers ************ */
/* ********************************* */

/*! \brief Helper function. Read config file*/
static struct ast_config* voise_load_asterisk_config(void)
{
    #if ASTERISK_VERSION_NUM < AST_6 
        return ast_config_load(VOISE_CFG); 
    #else
        struct ast_flags config_flags = { CONFIG_FLAG_WITHCOMMENTS };
        return ast_config_load(VOISE_CFG, config_flags);
    #endif 
}

/*! \brief Helper function. Test config file  */
static int __init_voise_res_speech(void)
{
    TRACE_FUNCTION();

    struct ast_config *vcfg;

    vcfg = voise_load_asterisk_config();

    if (!vcfg)
    {
        ast_log(LOG_ERROR, "Error opening configuration file %s\n", VOISE_CFG);
        return -1;
    }

    ast_config_destroy(vcfg);

    return 1;
}

static int __reinit_speech_controls(struct voise_speech_info *voise_info)
{
    TRACE_FUNCTION();

    CHECK_NOT_NULL(voise_info, "Voise info is NULL", -1);

    voise_info->heardspeech = 0;
    voise_info->noiseframes = 0;
    voise_info->start_time = 0;

    if (voise_info->dsp != NULL)
    {
        ast_dsp_free(voise_info->dsp);
        voise_info->dsp = NULL;
    }

    voise_info->dsp = ast_dsp_new();

    if (voise_info->dsp == NULL)
    {
        ast_log(LOG_ERROR, "Unable to create silence detection DSP\n");
        return -1;
    }

    ast_dsp_set_threshold(voise_info->dsp, VOISE_SILENCE_THRESHOLD);

    return 0;
}

/*! \brief Helper function. Set verbosity flag*/
static int __voise_set_verbose(struct ast_speech *speech, int v)
{
    TRACE_FUNCTION();

    CHECK_NOT_NULL(speech, "Speech is NULL", -1);

    struct voise_speech_info *voise_info;
    voise_info = (struct voise_speech_info*)speech->data;

    if (voise_info != NULL)
    {
        voise_info->verbose = v;
        return 0;
    }
    else
    {
        ast_log(LOG_ERROR, "Could not modify verbosity\n");
        return -1;
    }
}

/*! \brief Helper function. Get verbosity flag*/
static int __voise_get_verbose(struct ast_speech *speech)
{
    TRACE_FUNCTION();

    CHECK_NOT_NULL(speech, "Speech is NULL", -1);

    struct voise_speech_info *voise_info;
    voise_info = (struct voise_speech_info *)speech->data;

    if (voise_info != NULL)
    {
        return voise_info->verbose;
    }
    else
    {
        ast_log(LOG_ERROR, "Could not get verbosity\n");
        return -1;
    }
}

/*! \brief Helper function. Set language*/
static int __voise_set_lang(struct ast_speech *speech, const char *lang)
{
    TRACE_FUNCTION();

    CHECK_NOT_NULL(speech, "Speech is NULL", -1);

    struct voise_speech_info *voise_info;
    voise_info = (struct voise_speech_info *)speech->data;

    if (voise_info != NULL)
    {
        strncpy(voise_info->lang, lang, strlen(lang));
        voise_info->lang[strlen(lang)] = '\0';
        return 0;
    }
    else
    {
        ast_log(LOG_ERROR, "Could not set language to %s\n", lang);
        return -1;
    }
}

/*! \brief Helper function. Get language*/
static const char* __voise_get_lang(struct ast_speech *speech)
{
    TRACE_FUNCTION();

    CHECK_NOT_NULL(speech, "Speech is NULL", NULL);

    struct voise_speech_info *voise_info;
    voise_info = (struct voise_speech_info *)speech->data;

    if (voise_info != NULL)
        return voise_info->lang;
    else
    {
        ast_log(LOG_ERROR, "Could not get language\n");
        return NULL;
    }
}

/*! \brief Helper function. Set ASR engine*/
static int __voise_set_asr_engine(struct ast_speech *speech, const char *asr_engine)
{
    TRACE_FUNCTION();

    CHECK_NOT_NULL(speech, "Speech is NULL", -1);

    struct voise_speech_info *voise_info;
    voise_info = (struct voise_speech_info *)speech->data;

    if (voise_info != NULL)
    {
        strncpy(voise_info->asr_engine, asr_engine, strlen(asr_engine));
        voise_info->asr_engine[strlen(asr_engine)] = '\0';
        return 0;
    }
    else
    {
        ast_log(LOG_ERROR, "Could not set ASR engine to %s\n", asr_engine);
        return -1;
    }
}

/*! \brief Helper function. Get ASR engine*/
static const char* __voise_get_asr_engine(struct ast_speech *speech)
{
    TRACE_FUNCTION();

    CHECK_NOT_NULL(speech, "Speech is NULL", NULL);

    struct voise_speech_info *voise_info;
    voise_info = (struct voise_speech_info *)speech->data;

    if (voise_info != NULL)
        return voise_info->asr_engine;
    else
    {
        ast_log(LOG_ERROR, "Could not get ASR engine\n");
        return NULL;
    }
}

/*! \brief Helper function. Set model*/
static int __voise_set_model(struct ast_speech *speech, const char *model_name)
{
    TRACE_FUNCTION();

    CHECK_NOT_NULL(speech, "Speech is NULL", -1);

    struct voise_speech_info *voise_info;
    voise_info = (struct voise_speech_info *)speech->data;

    if (voise_info != NULL)
    {
        strncpy(voise_info->model_name, model_name, strlen(model_name));
        voise_info->model_name[strlen(model_name)] = '\0';
        return 0;
    }
    else
    {
        ast_log(LOG_ERROR, "Could not set model to %s\n", model_name);
        return -1;
    }
}

/*! \brief Helper function. Get model*/
static const char* __voise_get_model(struct ast_speech *speech)
{
    TRACE_FUNCTION();

    CHECK_NOT_NULL(speech, "Speech is NULL", NULL);

    struct voise_speech_info *voise_info;
    voise_info = (struct voise_speech_info *)speech->data;

    if (voise_info != NULL)
        return voise_info->model_name;
    else
    {
        ast_log(LOG_ERROR, "Could not get model\n");
        return NULL;
    }
}

/*! \brief Helper function. Set maximum initial silence*/
static int __voise_set_initsilence(struct ast_speech *speech, int initsil)
{
    TRACE_FUNCTION();

    CHECK_NOT_NULL(speech, "Speech is NULL", -1);

    struct voise_speech_info *voise_info;
    voise_info = (struct voise_speech_info *)speech->data;

    if (voise_info != NULL)
    {
        voise_info->initsil = initsil;
        return 0;
    }
    else
    {
        ast_log(LOG_ERROR, "Could not set maximum silence to %d\n", initsil);
        return -1;
    }
}

/*! \brief Helper function. Get initial silence*/
static int __voise_get_initsilence(struct ast_speech *speech)
{
    TRACE_FUNCTION();

    CHECK_NOT_NULL(speech, "Speech is NULL", -1);

    struct voise_speech_info *voise_info;
    voise_info = (struct voise_speech_info *)speech->data;

    if (voise_info != NULL)
        return voise_info->initsil;
    else
    {
        ast_log(LOG_ERROR, "Could not get initial silence\n");
        return -1;
    }
}

/*! \brief Helper function. Set maximum final silence*/
static int __voise_set_maxsilence(struct ast_speech *speech, int maxsil)
{
    TRACE_FUNCTION();

    CHECK_NOT_NULL(speech, "Speech is NULL", -1);

    struct voise_speech_info *voise_info;
    voise_info = (struct voise_speech_info *)speech->data;

    if (voise_info != NULL)
    {
        voise_info->maxsil = maxsil;
        return 0;
    }
    else
    {
        ast_log(LOG_ERROR, "Could not set maximum silence to %d\n", maxsil);
        return -1;
    }
}

/*! \brief Helper function. Get maximum silence*/
static int __voise_get_maxsilence(struct ast_speech *speech)
{
    TRACE_FUNCTION();

    CHECK_NOT_NULL(speech, "Speech is NULL", -1);

    struct voise_speech_info *voise_info;
    voise_info = (struct voise_speech_info *)speech->data;

    if (voise_info != NULL)
        return voise_info->maxsil;
    else
    {
        ast_log(LOG_ERROR, "Could not get maximum silence\n");
        return -1;
    }
}

/*! \brief Helper function. Set abs timeout*/
static int __voise_set_abstimeout(struct ast_speech *speech, int abs_timeout)
{
    TRACE_FUNCTION();

    CHECK_NOT_NULL(speech, "Speech is NULL", -1);

    struct voise_speech_info *voise_info;
    voise_info = (struct voise_speech_info *)speech->data;

    if (voise_info != NULL)
    {
        voise_info->abs_timeout = abs_timeout;
        return 0;
    }
    else
    {
        ast_log(LOG_ERROR, "Could not set abs timeout to %d\n", abs_timeout);
        return -1;
    }
}

/*! \brief Helper function. Get abs timeout*/
static int __voise_get_abstimeout(struct ast_speech *speech)
{
    TRACE_FUNCTION();

    CHECK_NOT_NULL(speech, "Speech is NULL", -1);

    struct voise_speech_info *voise_info;
    voise_info = (struct voise_speech_info *)speech->data;

    if (voise_info != NULL)
        return voise_info->abs_timeout;
    else
    {
        ast_log(LOG_ERROR, "Could not get abs timeout\n");
        return -1;
    }
}

/*! \brief Helper function. Set ASR result*/
static void __voise_set_result(struct ast_speech *speech, voise_response_t *voise_response)
{
    TRACE_FUNCTION();

    ast_speech_change_state(speech, AST_SPEECH_STATE_WAIT);

    int verbose = __voise_get_verbose(speech);

    int type_nbest = 0;

    if (speech->results_type == AST_SPEECH_RESULTS_TYPE_NBEST)
    {
        type_nbest = 1;

        if (verbose > 0)
            ast_log( LOG_NOTICE, "Nbest active (Max N=%d)\n", VOISE_MAX_NBEST);
    }

    struct ast_speech_result *result;

    if (speech->results == NULL)
        speech->results = ast_calloc(1, sizeof(struct ast_speech_result));

    result = speech->results;

    int ibest;
    for (ibest = 0; ibest < VOISE_MAX_NBEST; ++ibest)
    {
        result->score = (int)(voise_response->confidence * voise_response->probability * 100);
        result->text = ast_strndup(voise_response->utterance, strlen(voise_response->utterance));
        result->grammar = ast_strndup(voise_response->intent, strlen(voise_response->intent));

        if (!type_nbest)
            break;

        #if ASTERISK_VERSION_NUM < AST_6 
            result->next = ast_calloc(1, sizeof(struct ast_speech_result));
            result = result->next;
        #else
            result->list.next = ast_calloc(1, sizeof(struct ast_speech_result));
            result = result->list.next;
        #endif 
    }

    speech->flags = AST_SPEECH_HAVE_RESULTS;

    ast_speech_change_state(speech, AST_SPEECH_STATE_DONE);
}

/* ******************************************** */
/* ********* Speech API implementation ******** */
/* ******************************************** */

/*! \brief Find a speech recognition engine of specified name, if NULL then use the default one */
#if ASTERISK_VERSION_NUM < AST_6 
static int voise_create(struct ast_speech *speech)
#else
static int voise_create(struct ast_speech *speech, int format)
#endif
{
    TRACE_FUNCTION();

    CHECK_NOT_NULL(speech, "Speech is NULL", -1);

    if (speech->data == NULL)
    {
        speech->data = ast_calloc(1, sizeof(struct voise_speech_info));

        CHECK_NOT_NULL(speech->data, "Voise info is NULL", -1);
    }

    struct ast_config *vcfg = voise_load_asterisk_config();

    if (!vcfg) 
    {
        ast_log(LOG_ERROR, "Error opening configuration file %s\n", VOISE_CFG);
        return -1;
    }

    /* Verbosity */
    const char *vverbose;
    if ( !(vverbose = ast_variable_retrieve(vcfg, "debug", "verbose")))
        vverbose = VOISE_DEF_VERBOSE;

    if (vverbose != NULL)
        __voise_set_verbose(speech, atoi(vverbose));

    /* Default lang */
    const char *vlang;
    if ( !(vlang = ast_variable_retrieve(vcfg, "general", "lang")))
        vlang = VOISE_DEF_LANG;

    if (vlang != NULL)
        __voise_set_lang(speech, vlang);

    /* Default ASR engine */
    const char *vasrengine;
    if ( !(vasrengine = ast_variable_retrieve(vcfg, "general", "asr_engine")))
        vasrengine = VOISE_DEF_ASR_ENGINE;

    if (vasrengine != NULL)
        __voise_set_asr_engine(speech, vasrengine);    

    /* Default max initial silence */
    const char *vinitsil;
    if ( !(vinitsil = ast_variable_retrieve(vcfg, "general", "initsil")))
        vinitsil = VOISE_DEF_INIT_SIL; 

    if (vinitsil != NULL)
        __voise_set_initsilence(speech, atoi(vinitsil));

    /* Default max final silence */
    const char *vmaxsil;
    if ( !(vmaxsil = ast_variable_retrieve(vcfg, "general", "maxsil")))
        vmaxsil = VOISE_DEF_MAX_SIL;

    if (vmaxsil != NULL)
        __voise_set_maxsilence(speech, atoi(vmaxsil));

    /* Default abs timestou */
    const char *vabstimeout;
    if ( !(vabstimeout = ast_variable_retrieve(vcfg, "general", "abs_timeout")))
        vabstimeout = VOISE_DEF_ABS_TIMEOUT;

    if (vabstimeout != NULL)
        __voise_set_abstimeout(speech, atoi(vabstimeout));

    /* Server IP */
    const char *vserverip;
    if ( !(vserverip = ast_variable_retrieve(vcfg, "general", "serverip")) )
        vserverip = VOISE_DEF_HOST;

    struct voise_speech_info *voise_info;
    voise_info = (struct voise_speech_info *)speech->data;

    CHECK_NOT_NULL(voise_info, "Voise info is NULL", -1);

    voise_info->client = voise_init(vserverip, 8100, 1);

    if (voise_info->client == NULL)
    {
        ast_log(LOG_ERROR, "Could not connect to Voise server (%s).\n", vserverip);
        ast_config_destroy(vcfg);
        return -1;
    }

    ast_config_destroy(vcfg);

    ast_speech_change_state(speech, AST_SPEECH_STATE_NOT_READY);

    return 0;
}

/*! \brief  Destroy connection to engine. */
static int voise_destroy(struct ast_speech *speech)
{
    TRACE_FUNCTION();

    CHECK_NOT_NULL(speech, "Speech is NULL", -1);

    int verbose = __voise_get_verbose(speech);

    struct voise_speech_info *voise_info;
    voise_info = (struct voise_speech_info *)speech->data;
    
    CHECK_NOT_NULL(voise_info, "Voise info is NULL", -1);

    if (verbose)
        ast_log(LOG_NOTICE, "Closing connection to Voise server.\n");

    voise_close( voise_info->client );

    ast_free(voise_info);
    voise_info = NULL;
    
    return 0;
}

/*! \brief Load a local grammar on a speech structure */
static int voise_load_grammar(struct ast_speech *speech, char *grammar_name, char *grammar)
{
    TRACE_FUNCTION();

    // Do nothing
    return 0;
}

/*! \brief Unload a local grammar from a speech structure */
static int voise_unload_grammar(struct ast_speech *speech, char *grammar_name)
{
    TRACE_FUNCTION();

    // Do nothing
    return 0;
}

/*! \brief Activate a loaded (either local or global) grammar */
static int voise_activate_grammar(struct ast_speech *speech, char *grammar_name)
{
    TRACE_FUNCTION();

    CHECK_NOT_NULL(speech, "Speech is NULL", -1);

    int verbose = __voise_get_verbose(speech);

    if (verbose > 0)
        ast_log(LOG_NOTICE, "Activating grammar '%s'\n", grammar_name);

    return __voise_set_model(speech, grammar_name);
}

/*! \brief Deactivate a loaded grammar on a speech structure */
static int voise_deactivate_grammar(struct ast_speech *speech, char *grammar_name)
{
    TRACE_FUNCTION();

    CHECK_NOT_NULL(speech, "Speech is NULL", -1);

    int verbose = __voise_get_verbose(speech);

    if (verbose > 0)
        ast_log(LOG_NOTICE, "Deactivating grammar '%s'\n", grammar_name);

    return __voise_set_model(speech, "");
}

/*! \brief Write in signed linear audio to be recognized */
static int voise_write(struct ast_speech *speech, void *data, int len)
{
    TRACE_FUNCTION();

    CHECK_NOT_NULL(speech, "Speech is NULL", -1);

    struct voise_speech_info *voise_info;
    voise_info = (struct voise_speech_info *) speech->data;

    CHECK_NOT_NULL(voise_info, "Voise info is NULL", -1);

    int verbose = __voise_get_verbose(speech);

    int initsil = __voise_get_initsilence(speech);
    int maxsil = __voise_get_maxsilence(speech);
    int abs_timeout = __voise_get_abstimeout(speech);

    /* The Voise system doesn't seem be helpful in detecting silence and determing
     * the end of an utterance on its own, so here we use Asterisk's silence detection
     * DSP to fake sane behaviour. 
     * The Asterisk Generic Speech API strips the frame away from the data we are
     * sent, so to use the DSP, here we must re-create a frame.
     */
    struct ast_frame f;
    f.data.ptr = data;
    f.datalen = len;
    f.samples = len / 2;
    f.mallocd = 0;
    f.frametype = AST_FRAME_VOICE;
#if ASTERISK_VERSION_NUM == AST_13
    f.subclass.format = ast_format_slin;
#else
    ast_format_set(&f.subclass.format, AST_FORMAT_SLINEAR, 0);
#endif
    

    int totalsil;
    int silence = ast_dsp_silence(voise_info->dsp, &f, &totalsil);

    time_t current_time;
    time(&current_time);

    if (!voise_info->heardspeech && !silence)
    {
        voise_info->noiseframes++;

        if (voise_info->noiseframes > VOISE_NOISE_FRAMES)
        {
            if (verbose)
                ast_log(LOG_DEBUG, "Detected speech.\n");

            voise_info->heardspeech = 1;
            voise_info->noiseframes = 0;

            /* Stop sound file stream */
            speech->flags |= AST_SPEECH_QUIET;

            speech->flags |= AST_SPEECH_SPOKE;
        }
    }
    else if (!voise_info->heardspeech && silence && initsil >= 0 && initsil <= totalsil)
    {
        if (verbose)
            ast_log(LOG_NOTICE, "Maximum initial silence detected: %d.\n", totalsil);

        voise_response_t response;
        int ret = voise_stop_streaming_recognize( voise_info->client, &response );

        if (ret < 0)
        {
            ast_log(LOG_ERROR, "Streaming stop error: %d\n", ret);
            ast_speech_change_state(speech, AST_SPEECH_STATE_NOT_READY);
            
            return -1;
        }

        __voise_set_result( speech, &response );

        return 0;
    }
    else if (voise_info->heardspeech && silence && maxsil >= 0 && maxsil <= totalsil)
    {
        if (verbose)
            ast_log(LOG_NOTICE, "Maximum final silence detected: %d.\n", totalsil);

        voise_response_t response;
        int ret = voise_stop_streaming_recognize( voise_info->client, &response );

        if (ret < 0)
        {
            ast_log(LOG_ERROR, "Streaming stop error: %d\n", ret);
            ast_speech_change_state(speech, AST_SPEECH_STATE_NOT_READY);
            
            return -1;
        }

        __voise_set_result( speech, &response );

        return 0;
    }
    else if (abs_timeout > 0 && abs_timeout <= (current_time - voise_info->start_time))
    {
        if (verbose)
            ast_log(LOG_NOTICE, "Absolute timeout reached [%d seconds].\n", (int)(current_time - voise_info->start_time));

        voise_response_t response;
        int ret = voise_stop_streaming_recognize( voise_info->client, &response );

        if (ret < 0)
        {
            ast_log(LOG_ERROR, "Streaming stop error: %d\n", ret);
            ast_speech_change_state(speech, AST_SPEECH_STATE_NOT_READY);

            return -1;
        }

        __voise_set_result( speech, &response );

        return 0;
    }
    else if (silence)
    {
        voise_info->noiseframes = 0;
    }

#ifdef TRACE_ENABLED
        ast_log(LOG_DEBUG, ">>>> heardspeech: %d | silence: %d | totalsil: %d | noiseframes: %d | <<<<\n", voise_info->heardspeech, silence, totalsil, voise_info->noiseframes);
#endif

    int ret = voise_data_streaming_recognize( voise_info->client, data, len );

    if (ret < 0)
    {
        ast_log(LOG_ERROR, "Streaming data error: %d\n", ret);
        ast_speech_change_state(speech, AST_SPEECH_STATE_NOT_READY);

        return -1;
    }

    return 0;
}

/*! \brief Signal to the engine that DTMF was received */
static int voise_dtmf(struct ast_speech *speech, const char *dtmf)
{
    TRACE_FUNCTION();

    ast_log(LOG_NOTICE, "Voise dtmf not implemented\n");

    return 0;
}

/*! \brief Start speech recognition on a speech structure */
static int voise_start(struct ast_speech *speech)
{
    TRACE_FUNCTION();

    CHECK_NOT_NULL(speech, "Speech is NULL", -1);

    int verbose = __voise_get_verbose(speech);

    struct voise_speech_info *voise_info;
    voise_info = (struct voise_speech_info *) speech->data;

    if (__reinit_speech_controls(voise_info) < 0)
        return -1;

    CHECK_NOT_NULL(voise_info, "Voise info is NULL", -1);

    const char *lang = __voise_get_lang(speech);
    const char *asr_engine = __voise_get_asr_engine(speech);
    const char *model_name = __voise_get_model(speech);

    if (verbose)
    {
        ast_log(LOG_VERBOSE, "Start recognize:\n  Lang: %s\n  Model name: %s\n  ASR engine: %s\n", 
            lang, model_name, asr_engine);
    }

    voise_response_t response;
    int ret = voise_start_streaming_recognize(
        voise_info->client, &response, "LINEAR16", 8000, lang, NULL, model_name, asr_engine);

    if (ret < 0)
    {
        ast_log(LOG_ERROR, "Streaming start error: %d\n", ret);
        return -1;
    }

    if (response.result_code != 201)
    {
        ast_log(LOG_ERROR, "Streaming not started: %s\n", response.result_message);
        return -1;
    }

    time(&voise_info->start_time);

    /* Voise engine is ready to accept samples */
    ast_speech_change_state(speech, AST_SPEECH_STATE_READY);

    if (verbose)
        ast_log(LOG_DEBUG, "Streaming started.\n");

    return 0;
}

/*! \brief Change an engine specific attribute */
static int voise_change(struct ast_speech *speech, char *name, const char *value)
{
    TRACE_FUNCTION();

    CHECK_NOT_NULL(speech, "Speech is NULL", -1);

    int retval = 0;
    int verbose = __voise_get_verbose(speech);

    if (verbose > 0)
        ast_log(LOG_NOTICE, "Setting attribute \'%s\' to \'%s\'\n", name, value);

    if (!strcmp(name, "verbose"))
    {
        __voise_set_verbose(speech, atoi(value));
    }
    else if (!strcmp(name, "language"))
    {
        if (__voise_set_lang(speech, value) < 0)
            retval = -1;
    }
    else if (!strcmp(name, "lang"))
    {
        if (__voise_set_lang(speech, value) < 0)
            retval = -1;
    }
    else if (!strcmp(name, "asr_engine"))
    {
        if (__voise_set_asr_engine(speech, value) < 0)
            retval = -1;
    }
    else if (!strcmp(name, "initsil"))
    {
        if (__voise_set_initsilence(speech, atoi(value)) < 0)
            retval = -1;
    }
    else if (!strcmp(name, "maxsil"))
    {
        if (__voise_set_maxsilence(speech, atoi(value)) < 0)
            retval = -1;
    }
    else if (!strcmp(name, "abs_timeout"))
    {
        if (__voise_set_abstimeout(speech, atoi(value)) < 0)
            retval = -1;
    }
    else
    {
        ast_log(LOG_WARNING, "Unknown attribute %s\n", name);
    }

    return retval;
}

/*! \brief  Change the type of results we want back  */
static int voise_change_results_type(struct ast_speech *speech, enum ast_speech_results_type results_type)
{
    TRACE_FUNCTION();

    if (results_type == AST_SPEECH_RESULTS_TYPE_NBEST)
    {
        ast_log(LOG_NOTICE, "Voise change results to nbest\n");
    }

    return 0;
}

/*! \brief Try to get results */
static struct ast_speech_result* voise_get(struct ast_speech *speech)
{
    TRACE_FUNCTION();

    CHECK_NOT_NULL(speech, "Speech is NULL", NULL);

    return speech->results;
}

static struct ast_speech_engine voise_engine = {
    .name = "voise",
    .create = voise_create,
    .destroy = voise_destroy,
    .load = voise_load_grammar,
    .unload = voise_unload_grammar,
    .activate = voise_activate_grammar,
    .deactivate = voise_deactivate_grammar,
    .write = voise_write,
    .dtmf = voise_dtmf,
    .start = voise_start,
    .change = voise_change,
    .change_results_type = voise_change_results_type,
    .get = voise_get,
};

static int load_module(void)
{
    ast_log(LOG_NOTICE, "Loading Voise resourse module\n");

    if (__init_voise_res_speech() == 1)
    {

#if ASTERISK_VERSION_NUM >= AST_13
        voise_engine.formats = ast_format_cap_alloc(AST_FORMAT_CAP_FLAG_DEFAULT);

        if (!voise_engine.formats)
        {
            ast_log(LOG_ERROR, "Failed to alloc media format capabilities\n");
            return AST_MODULE_LOAD_FAILURE;
        }

        ast_format_cap_append(voise_engine.formats, ast_format_slin, 0);
#elif ASTERISK_VERSION_NUM >= AST_10 && ASTERISK_VERSION_NUM < AST_13 /* 11-12 */
        struct ast_format format;
        ast_format_set(&format, AST_FORMAT_SLINEAR, 0);

        if (!(voise_engine.formats = ast_format_cap_alloc(0)))
        {
            ast_log(LOG_ERROR, "Failed to alloc media format capabilities\n");
            return AST_MODULE_LOAD_FAILURE;
        }
        ast_format_cap_add(voise_engine.formats, &format);
#else /* <= 1.8 */
        voise_engine.formats = AST_FORMAT_SLINEAR;
#endif

        if (ast_speech_register(&voise_engine))
        {
            ast_log(LOG_ERROR, "Failed to register Voise resource module\n");    
            return -1;
        }

        return 0;
    }
    else 
        return 0;/*do not stop asterisk startup*/
}

static int unload_module(void)
{
    ast_log(LOG_NOTICE, "Unloading Voise resourse speech\n");

    return ast_speech_unregister(voise_engine.name);
}

AST_MODULE_INFO_STANDARD(ASTERISK_GPL_KEY, "Voise engine");