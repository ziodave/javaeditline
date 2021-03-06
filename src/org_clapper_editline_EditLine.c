/*---------------------------------------------------------------------------*\
  This software is released under a BSD license, adapted from
  http://opensource.org/licenses/bsd-license.php.

  Copyright (c) 2010 Brian M. Clapper
  All rights reserved.

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions are met:
  
  * Redistributions of source code must retain the above copyright notice,
    this list of conditions and the following disclaimer.

  * Redistributions in binary form must reproduce the above copyright notice,
    this list of conditions and the following disclaimer in the documentation
    and/or other materials provided with the distribution.

  * Neither the names "clapper.org", "Java EditLine", nor the names of its
    contributors may be used to endorse or promote products derived from
    this software without specific prior written permission.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
  POSSIBILITY OF SUCH DAMAGE.
\*---------------------------------------------------------------------------*/

#include <jni.h>
#include <stdio.h>
#include <histedit.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <assert.h>

#include "org_clapper_editline_EditLine.h"

#define min(a, b)  ((a) < (b) ? (a) : (b))

#define PROMPT_MAX 128

#define elPointer2jlong(handle) ((jlong) ((long) handle)) 
#define jlong2elPointer(jl) ((EditLine *) ((long) jl))

typedef struct _SimpleStringList
{
    const char *string;
    struct _SimpleStringList *next;
}
SimpleStringList;

typedef struct _jEditLineData
{
    char prompt[PROMPT_MAX];
    History *history;
    JNIEnv *env;
    jclass javaClass;
    jobject javaEditLine;
    jmethodID handleCompletionMethodID;
    jmethodID showCompletionsMethodID;
    jint max_history_size;
}
jEditLineData;

static jEditLineData *get_data(EditLine *el)
{
    void *d;
    el_get(el, EL_CLIENTDATA, &d);
    return (jEditLineData *) d;
}

/**
 * Determine the common first substring of an array of strings.
 * Returns a malloc'd string the caller must free.
 */
static char *common_first_substring(JNIEnv *env,
                                    jobjectArray strings,
                                    int len,
                                    int start)
{
    char *result = NULL;
    jstring js;
    const char *first;

    if (len == 0)
        ;

    else if (len == 1)
    {
        js = (jstring) (*env)->GetObjectArrayElement(env, strings, start);
        first = (*env)->GetStringUTFChars(env, js, NULL);
        result = strdup(first);
        (*env)->ReleaseStringUTFChars(env, js, first);
    }

    else
    {
        char *subcommon = common_first_substring(env,
                                                 strings,
                                                 len - 1,
                                                 start + 1);
        if (subcommon == NULL)
            result = NULL;

        else
        {
            js = (jstring) (*env)->GetObjectArrayElement(env, strings, start);
            first = (*env)->GetStringUTFChars(env, js, NULL);
            int first_len = strlen(first);
            int subcommon_len = strlen(subcommon);

            if ((first_len == 0) || (subcommon_len == 0))
                result = NULL;

            else
            {
                int shortest = min(subcommon_len, first_len);
                int i;
                int total = 0;
                char *s;
                const char *f;
                for (f = first, s = subcommon, i = 0;
                     i < shortest;
                     i++, s++, f++)
                {
                    if (*f != *s)
                        break;
                    total++;
                }

                if (total > 0)
                {
                    result = (char *) malloc(sizeof(char) * total + 1);
                    strncpy(result, first, total);
                    result[total] = '\0';
                }
            }

            free(subcommon);
            (*env)->ReleaseStringUTFChars(env, js, first);
        }
    }

    return result;
}

static void set_prompt(EditLine *el, const char *new_prompt)
{
    jEditLineData *data = get_data(el);
    strncpy(data->prompt, new_prompt, PROMPT_MAX - 1);
}

static const char *get_prompt(EditLine *el)
{
    jEditLineData *data = get_data(el);
    return data->prompt;
}

static void replace_token(EditLine *el, int token_len, const char *new_token)
{
    el_deletestr(el, token_len);
    el_insertstr(el, new_token);
}

static unsigned char 
use_first_completion(JNIEnv *env,
                     EditLine *el,
                     jobjectArray completions,
                     int token_len)
{
    unsigned char result = CC_ERROR;

    jstring js = (jstring) (*env)->GetObjectArrayElement(env, completions, 0);
    const char *cs = (*env)->GetStringUTFChars(env, js, NULL);
    if (cs == NULL)
        fputs("Out of memory (Java) during completion.\n", stderr);

    else
    {
        result = CC_REFRESH;
        replace_token(el, token_len, cs);
        (*env)->ReleaseStringUTFChars(env, js, cs);
    }

    return result;
}

static unsigned char
show_completions(JNIEnv *env, EditLine *el, jobjectArray completions)
{
    jEditLineData *data = get_data(el);
    jmethodID method = data->showCompletionsMethodID;
    (*env)->CallObjectMethod(env, data->javaEditLine, method, completions);
    return CC_REDISPLAY;
}

static unsigned char complete(EditLine *el, int ch)
{
    jEditLineData *data = get_data(el);
    JNIEnv *env = data->env;

    const LineInfo *lineInfo = el_line(el);

    jstring jLine;
    jint jCursor;
    jstring jToken;
    int token_len = 0;
    
    if (lineInfo != NULL)
    {
        int line_len = (int) (lineInfo->lastchar - lineInfo->buffer);
        char *line = (char *) malloc(sizeof(char) * (line_len + 1));
        (void) strncpy(line, lineInfo->buffer, line_len);
        line[line_len] = '\0';

        jLine = (*env)->NewStringUTF(env, line);
        free(line);

        /* Find the beginning of the current token */

        const char *ptr;
	for (ptr = lineInfo->cursor - 1; ptr > lineInfo->buffer; ptr--)
        {
            if (isspace((unsigned char) *ptr))
            {
                ptr++;
                break;
            }
        }

	token_len = lineInfo->cursor - ptr;

        /* Save it as a Java string. */

        if (token_len == 0)
            jToken = (*env)->NewStringUTF(env, "");

        else
        {
            char *token = (char *) malloc(token_len + 1);
            strncpy(token, ptr, token_len);
            token[token_len] = '\0';
            jToken = (*env)->NewStringUTF(env, token);
            free(token);
        }

        jCursor = (long) (lineInfo->cursor - lineInfo->buffer);
    }

    else
    {
        jToken = (*env)->NewStringUTF(env, "");
        jLine = (*env)->NewStringUTF(env, "");
        jCursor = 0;
    }

    jmethodID method = data->handleCompletionMethodID;

    unsigned char result = CC_ERROR;
    jobjectArray completions = (*env)->CallObjectMethod(env,
                                                        data->javaEditLine,
                                                        method,
                                                        jToken,
                                                        jLine,
                                                        jCursor);

    if (completions != NULL)
    {
        jint len = (*env)->GetArrayLength(env, completions);

        const char *s;
        switch ((int) len)
        {
            case 0:
                break;

            case 1:
                result = use_first_completion(env, el, completions, token_len);
                break;

            default:
                result = show_completions(env, el, completions);
                s = common_first_substring(env, completions, len, 0);
                if (s != NULL)
                    replace_token(el, token_len, s);
                break;
        }
    }

    return result;
}

/*
 * Class:  org_clapper_editline_EditLine
 * Method: static long n_el_init(String program, EditLine javaEditLine)
 */
JNIEXPORT jlong JNICALL Java_org_clapper_editline_EditLine_n_1el_1init
    (JNIEnv *env, jclass cls, jstring program, jobject javaEditLine)
{
    const char *cProgram = (*env)->GetStringUTFChars(env, program, NULL);
    if (cProgram == NULL)
    {
        /* OutOfMemoryError already thrown */
        return;
    }

    jEditLineData *data = (jEditLineData*) malloc(sizeof(jEditLineData));
    jlong handle = 0;

    if (data == NULL)
    {
        jclass exc = (*env)->FindClass(env, "java/lang/OutOfMemoryError");
        (*env)->ThrowNew(env, exc, "unable to allocation jEditLineData");
    }

    else
    {
        EditLine *el = el_init(cProgram, stdin, stdout, stderr);
        data->history = history_init();

        data->env = env;
        data->javaClass = (*env)->NewGlobalRef(env, cls);
        data->javaEditLine = (*env)->NewGlobalRef(env, javaEditLine);
        data->max_history_size = 0;
        data->handleCompletionMethodID = (*env)->GetMethodID(
            env, cls, "handleCompletion",
            "(Ljava/lang/String;Ljava/lang/String;I)[Ljava/lang/String;");
        data->showCompletionsMethodID = (*env)->GetMethodID(
            env, cls, "showCompletions",
            "([Ljava/lang/String;)V");
        el_set(el, EL_ADDFN, "ed-complete", "Complete", complete);
        el_set(el, EL_CLIENTDATA, (void *) data);
        el_set(el, EL_PROMPT, get_prompt);
        el_set(el, EL_HIST, history, data->history);
        el_set(el, EL_SIGNAL, 1);
        handle = elPointer2jlong(el);
    }

    (*env)->ReleaseStringUTFChars(env, program, cProgram);
    return handle;
}

/*
 * Class:  org_clapper_editline_EditLine
 * Method: static void n_el_source(long handle, String path)
 */
JNIEXPORT void JNICALL Java_org_clapper_editline_EditLine_n_1el_1source
    (JNIEnv *env, jclass cls, jlong handle, jstring javaPath)
{
    EditLine *el = jlong2elPointer(handle);
    if (javaPath == NULL)
        el_source(el, NULL);

    else
    {
        const char *path = (*env)->GetStringUTFChars(env, javaPath, NULL);
        if (path == NULL)
        {
            /* OutOfMemoryError already thrown */
        }

        else
        {
            el_source(el, path);
            (*env)->ReleaseStringUTFChars(env, javaPath, path);
        }
    }
}


/*
 * Class:  org_clapper_editline_EditLine
 * Method: static void n_el_end(long handle)
 */
JNIEXPORT void JNICALL Java_org_clapper_editline_EditLine_n_1el_1end
    (JNIEnv *env, jclass cls, jlong handle)
{
    EditLine *el = jlong2elPointer(handle);
    jEditLineData *data = get_data(el);
    history_end(data->history);
    el_end(el);
    el = NULL;
    free(data);
}

/*
 * Class:  org_clapper_editline_EditLine
 * Method: static void n_el_set_prompt(long handle, String prompt)
 */
JNIEXPORT void JNICALL Java_org_clapper_editline_EditLine_n_1el_1set_1prompt
    (JNIEnv *env, jclass cls, jlong handle, jstring prompt)
{
    const char *str = (*env)->GetStringUTFChars(env, prompt, NULL);
    if (str == NULL)
    {
        /* OutOfMemoryError already thrown */
    }

    else
    {
        EditLine *el = jlong2elPointer(handle);
        set_prompt(el, str);
        (*env)->ReleaseStringUTFChars(env, prompt, str);
    }
}

/*
 * Class:  org_clapper_editline_EditLine
 * Method: static String n_el_gets(long handle)
 */
JNIEXPORT jstring JNICALL Java_org_clapper_editline_EditLine_n_1el_1gets
    (JNIEnv *env, jclass cls, jlong handle)
{
    jstring result = NULL;
    EditLine *el = jlong2elPointer(handle);
    int count;
    const char *line = el_gets(el, &count);
    if (line != NULL)
        result = (*env)->NewStringUTF(env, line);

    return result;
}

/*
 * Class:  org_clapper_editline_EditLine
 * Method: static void n_el_bind(long handle, String[] args)
 */
JNIEXPORT void JNICALL Java_org_clapper_editline_EditLine_n_1el_1parse
    (JNIEnv *env, jclass cls, jlong handle, jobjectArray args, jint len)
{
    EditLine *el = jlong2elPointer(handle);
    const char **buf = (const char **) malloc(len * sizeof(const char *));
    const char **ptr;
    int i;
    jstring js;

    for (i = 0, ptr = buf; i < (int) len; i++, ptr++)
    {
        js = (jstring) (*env)->GetObjectArrayElement(env, args, i);
        const char *cs = (*env)->GetStringUTFChars(env, js, NULL);
        *ptr = cs;
    }

    el_parse(el, (int) len, buf);

    for (ptr = buf, i = 0; i < (int) len; i++, ptr++)
    {
        js = (jstring) (*env)->GetObjectArrayElement(env, args, i);
        (*env)->ReleaseStringUTFChars(env, js, *ptr);
    }
}

/*
 * Class:  org_clapper_editline_EditLine
 * Method: static int n_history_get_size(long handle)
 */
JNIEXPORT jint JNICALL Java_org_clapper_editline_EditLine_n_1history_1get_1size
    (JNIEnv *env, jclass cls, jlong handle)
{
    EditLine *el = jlong2elPointer(handle);
    jEditLineData *data = get_data(el);

    return data->max_history_size;
}

/*
 * Class:  org_clapper_editline_EditLine
 * Method: static void n_history_set_size(long handle, int size)
 */
JNIEXPORT void JNICALL Java_org_clapper_editline_EditLine_n_1history_1set_1size
    (JNIEnv *env, jclass cls, jlong handle, jint newSize)
{
    EditLine *el = jlong2elPointer(handle);
    jEditLineData *data = get_data(el);
    HistEvent ev;
    history(data->history, &ev, H_SETSIZE, (int) newSize);
    data->max_history_size = newSize;
}

/*
 * Class:  org_clapper_editline_EditLine
 * Method: static void n_history_clear(long handle)
 */
JNIEXPORT void JNICALL Java_org_clapper_editline_EditLine_n_1history_1clear
    (JNIEnv *env, jclass cls, jlong handle)
{
    EditLine *el = jlong2elPointer(handle);
    HistEvent ev;
    jEditLineData *data = get_data(el);
    history(data->history, &ev, H_CLEAR);
}

/*
 * Class:  org_clapper_editline_EditLine
 * Method: static void n_history_append(long handle, String line)
 */
JNIEXPORT void JNICALL Java_org_clapper_editline_EditLine_n_1history_1append
    (JNIEnv *env, jclass cls, jlong handle, jstring line)
{
    const char *str = (*env)->GetStringUTFChars(env, line, NULL);
    if (str == NULL)
    {
        /* OutOfMemoryError already thrown */
    }

    else
    {
        EditLine *el = jlong2elPointer(handle);
        HistEvent ev;
        ev.str = str;
        jEditLineData *data = get_data(el);
        history(data->history, &ev, H_ENTER, str);
        (*env)->ReleaseStringUTFChars(env, line, str);
    }
}

/*
 * Class:  org_clapper_editline_EditLine
 * Method: static String n_history_current(long handle)
 */
JNIEXPORT jstring JNICALL Java_org_clapper_editline_EditLine_n_1history_1current
  (JNIEnv *env, jclass cls, jlong handle)
{
    EditLine *el = jlong2elPointer(handle);
    jEditLineData *data = get_data(el);
    jstring result = NULL;
    HistEvent ev;

    if (history(data->history, &ev, H_FIRST) != -1)
        result = (*env)->NewStringUTF(env, ev.str);

    return result;
}

/*
 * Class:  org_clapper_editline_EditLine
 * Method: static String[] n_history_get_all(long handle)
 */
JNIEXPORT
jobjectArray JNICALL Java_org_clapper_editline_EditLine_n_1history_1get_1all
    (JNIEnv *env, jclass cls, jlong handle)
{
    SimpleStringList *list = NULL;

    int total = 0;
    int rc;
    HistEvent ev;

    /*
      Capture the history elements in a linked-list, so we only have to
      traverse the list once. For simplicity, insert the entries at the top
      (like a stack).
    */
    EditLine *el = jlong2elPointer(handle);
    jEditLineData *data = get_data(el);
    for (rc = history(data->history, &ev, H_LAST);
         rc != -1;
         rc = history(data->history, &ev, H_PREV))
    {
        SimpleStringList *entry =
            (SimpleStringList *) malloc(sizeof(SimpleStringList));

        if (list == NULL)
        {
            list = entry;
            entry->next = NULL;
        }
        else
        {
            entry->next = list;
            list = entry;
        }

        entry->string = strdup(ev.str);
        total++;
    }

    /* Allocate an appropriate size array. */

    jobjectArray result = (jobjectArray)
        (*env)->NewObjectArray(env, total,
                               (*env)->FindClass(env, "java/lang/String"),
                               (*env)->NewStringUTF(env, ""));

    /*
      Traverse the list and fill the array backwards, since the list
      is in reverse order.
    */
    int i = total;
    SimpleStringList *entry = list;
    SimpleStringList *prev = NULL;
    while (entry != NULL)
    {
        i--;
        assert (i >= 0);
        assert(entry->string != NULL);

        /* Create a Java version of the string. */
        jstring js = (*env)->NewStringUTF(env, entry->string);

        /* Store it in the Java array. */
        (*env)->SetObjectArrayElement(env, result, i, js);

        /* Move along. */
        prev = entry;
        entry = entry->next;

        /* Free the linked list entry and its string. */
        free((void *) prev->string);
        free(prev);
    }

    return result;
}

/*
 * Class:     org_clapper_editline_EditLine
 * Method:    static void n_history_set_unique(long handle, boolean on)
 * Signature: (Z)V
 */
JNIEXPORT void
JNICALL Java_org_clapper_editline_EditLine_n_1history_1set_1unique
    (JNIEnv *env, jclass cls, jlong handle, jboolean on)
{
    EditLine *el = jlong2elPointer(handle);
    jEditLineData *data = get_data(el);
    HistEvent ev;
    history(data->history, &ev, H_SETUNIQUE, on ? 1 : 0);
}
