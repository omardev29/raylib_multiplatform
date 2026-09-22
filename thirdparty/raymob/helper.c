/*
 *  raymob License (MIT)
 *
 *  Copyright (c) 2023-2024 Le Juez Victor
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to deal
 *  in the Software without restriction, including without limitation the rights
 *  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *  copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *  SOFTWARE.
 */

#include "raymob.h"
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#ifdef __ANDROID__

/* Static variables */

static jobject featuresInstance = NULL;

/* [rmp patch] Attach bookkeeping, per thread.
 *
 * raylib's game loop runs on the thread android_native_app_glue created, and
 * that thread is attached to the VM for the life of the process. Attaching an
 * attached thread is a no-op that hands back the existing JNIEnv, but
 * DetachCurrentThread on it is NOT a no-op: it detaches for real, invalidating
 * every local reference the frame above still holds and forcing the next call
 * to re-attach with a different JNIEnv.
 *
 * So only a thread THIS code attached may be detached, and only when the
 * outermost pair unwinds -- the helpers nest (GetScreenOrientation attaches,
 * and so does anything it calls).
 */
static __thread int attachDepth = 0;
static __thread bool attachedHere = false;

/* Functions definition */

JNIEnv* AttachCurrentThread(void)
{
    JavaVM *vm = GetAndroidApp()->activity->vm;
    JNIEnv *env = NULL;

    if ((*vm)->GetEnv(vm, (void**)&env, JNI_VERSION_1_6) == JNI_OK)
    {
        attachDepth++;  // Already attached: borrow it, and detach nothing
        return env;
    }

    if ((*vm)->AttachCurrentThread(vm, &env, NULL) != JNI_OK) return NULL;

    if (attachDepth == 0) attachedHere = true;
    attachDepth++;

    return env;
}

void DetachCurrentThread(void)
{
    if (attachDepth > 0) attachDepth--;
    if (attachDepth > 0 || !attachedHere) return;

    JavaVM *vm = GetAndroidApp()->activity->vm;
    (*vm)->DetachCurrentThread(vm);
    attachedHere = false;
}

bool RaymobExceptionCheck(JNIEnv *env, const char *where)
{
    if (env == NULL) return false;
    if (!(*env)->ExceptionCheck(env)) return false;

    (*env)->ExceptionDescribe(env);  // The trace goes to logcat, and nowhere else
    (*env)->ExceptionClear(env);
    TraceLog(LOG_WARNING, "RAYMOB: Java exception in '%s' (cleared)", where);

    return true;
}

jmethodID RaymobGetMethod(JNIEnv *env, jobject object, const char *name, const char *sig)
{
    if (env == NULL || object == NULL) return NULL;

    jclass cls = (*env)->GetObjectClass(env, object);
    jmethodID method = (*env)->GetMethodID(env, cls, name, sig);
    (*env)->DeleteLocalRef(env, cls);

    if (method == NULL)
    {
        // A pending NoSuchMethodError aborts the VM at the next JNI call from
        // anywhere, so it is cleared here rather than left for someone else
        RaymobExceptionCheck(env, name);
        TraceLog(LOG_WARNING, "RAYMOB: Method not found: %s%s", name, sig);
    }

    return method;
}

jobject RaymobGetObjectField(JNIEnv *env, jobject object, const char *name, const char *sig)
{
    if (env == NULL || object == NULL) return NULL;

    jclass cls = (*env)->GetObjectClass(env, object);
    jfieldID field = (*env)->GetFieldID(env, cls, name, sig);
    (*env)->DeleteLocalRef(env, cls);

    if (field == NULL)
    {
        RaymobExceptionCheck(env, name);
        TraceLog(LOG_WARNING, "RAYMOB: Field not found: %s %s", sig, name);
        return NULL;
    }

    return (*env)->GetObjectField(env, object, field);
}

jobject GetNativeLoaderInstance(void)
{
    return GetAndroidApp()->activity->clazz;
}

jobject GetFeaturesInstance(void)
{
    if (featuresInstance == NULL)
    {
        JNIEnv *env = AttachCurrentThread();
        jobject nativeLoaderInstance = GetNativeLoaderInstance();

        jmethodID getFeaturesMethod = RaymobGetMethod(env, nativeLoaderInstance, "getFeatures",
                                                      "()Lcom/raylib/raymob/Features;");

        // [rmp patch] the early return upstream took here skipped the detach
        if (getFeaturesMethod != NULL)
        {
            jobject localFeaturesInstance = (*env)->CallObjectMethod(env, nativeLoaderInstance, getFeaturesMethod);

            if (!RaymobExceptionCheck(env, "getFeatures") && localFeaturesInstance != NULL)
            {
                featuresInstance = (*env)->NewGlobalRef(env, localFeaturesInstance);
            }
        }

        DetachCurrentThread();
    }

    return featuresInstance;
}

char* GetCacheDir(void)
{
    struct android_app *app = GetAndroidApp();

    // [rmp patch] through the helpers, so a thread the VM attached is not
    // detached here, and so a missing method is a warning and not an abort
    JNIEnv* env = AttachCurrentThread();
    if (env == NULL) return NULL;

    // Get the activity object
    jobject activity = app->activity->clazz;

    // Get the method ID for the getCacheDir() method of the activity
    jmethodID getCacheDirMethod = RaymobGetMethod(env, activity, "getCacheDir", "()Ljava/io/File;");

    if (getCacheDirMethod == NULL)
    {
        DetachCurrentThread();
        return NULL;
    }

    // Call the getCacheDir() method to get the cache directory
    jobject cacheDir = (*env)->CallObjectMethod(env, activity, getCacheDirMethod);

    if (RaymobExceptionCheck(env, "getCacheDir") || cacheDir == NULL)
    {
        DetachCurrentThread();
        return NULL;
    }

    // Get the method ID for the getPath() method of java.io.File
    jmethodID getPathMethod = RaymobGetMethod(env, cacheDir, "getPath", "()Ljava/lang/String;");

    if (getPathMethod == NULL)
    {
        (*env)->DeleteLocalRef(env, cacheDir);
        DetachCurrentThread();
        return NULL;
    }

    // Call the getPath() method to get the path of the cache directory
    jstring pathString = (jstring)(*env)->CallObjectMethod(env, cacheDir, getPathMethod);

    if (RaymobExceptionCheck(env, "getPath") || pathString == NULL)
    {
        (*env)->DeleteLocalRef(env, cacheDir);
        DetachCurrentThread();
        return NULL;
    }

    // Get the UTF-8 encoded string from the Java string
    const char *pathChars = (*env)->GetStringUTFChars(env, pathString, NULL);

    if (pathChars == NULL)
    {
        (*env)->DeleteLocalRef(env, pathString);
        (*env)->DeleteLocalRef(env, cacheDir);
        DetachCurrentThread();
        return NULL;
    }

    // Allocate memory for the cache path
    size_t len = strlen(pathChars) + 1; // NOTE: +1 for the null terminator
    char* cachePath = RL_MALLOC(len);

    // Copy the string to the allocated memory
    if (cachePath)
    {
        strncpy(cachePath, pathChars, len);
        cachePath[len - 1] = '\0'; // NOTE: just for security
    }

    // Release the UTF-8 encoded string
    (*env)->ReleaseStringUTFChars(env, pathString, pathChars);

    // Clean up local references
    (*env)->DeleteLocalRef(env, pathString);
    (*env)->DeleteLocalRef(env, cacheDir);

    // Detach the current thread from the JavaVM
    DetachCurrentThread();

    // Return the cache path
    return cachePath;
}

char* LoadCacheFile(const char* fileName)
{
    char *text = NULL;
    char *cacheDir = GetCacheDir();

    if (cacheDir == NULL) return NULL;

    // [rmp patch] dir + '/' + name + '\0'. Upstream allocated one byte less
    // than that and then wrote the terminator at filePath[len], one past the
    // end of the block: an immediate abort under scudo, which is Android's
    // allocator since API 30, and silent heap corruption under a quieter one
    size_t len = strlen(cacheDir) + 1 + strlen(fileName) + 1;
    char *filePath = RL_MALLOC(len);

    if (filePath == NULL)
    {
        RL_FREE(cacheDir);
        return NULL;
    }

    snprintf(filePath, len, "%s/%s", cacheDir, fileName);

    FILE * file = fopen(filePath, "rt");
    if (file != NULL)
    {
        // WARNING: When reading a file as 'text' file,
        // text mode causes carriage return-linefeed translation...
        // ...but using fseek() should return correct byte-offset
        fseek(file, 0, SEEK_END);
        unsigned int size = (unsigned int)ftell(file);
        fseek(file, 0, SEEK_SET);

        if (size > 0)
        {
            text = (char *)RL_MALLOC((size + 1)*sizeof(char));

            if (text != NULL)
            {
                unsigned int count = (unsigned int)fread(text, sizeof(char), size, file);

                // WARNING: \r\n is converted to \n on reading, so,
                // read bytes count gets reduced by the number of lines
                if (count < size) text = RL_REALLOC(text, count + 1);

                // Zero-terminate the string
                text[count] = '\0';

                TraceLog(LOG_INFO, "FILEIO: [%s] Text file loaded successfully", fileName);
            }
            else TraceLog(LOG_WARNING, "FILEIO: [%s] Failed to allocated memory for file reading", fileName);
        }
        else TraceLog(LOG_WARNING, "FILEIO: [%s] Failed to read text file", fileName);

        fclose(file);
    }
    else TraceLog(LOG_WARNING, "FILEIO: File name provided is not valid");

    RL_FREE(filePath);
    RL_FREE(cacheDir);

    return text;
}

char* GetL10NString(const char* value)
{
    jobject nativeInstance = GetNativeLoaderInstance();

    if (nativeInstance != NULL)
    {
        JNIEnv* env = AttachCurrentThread();

        if (env == NULL) return NULL;

        // Get the native instance's getResources method
        jmethodID getResourcesMethod = RaymobGetMethod(env, nativeInstance, "getResources",
                                                       "()Landroid/content/res/Resources;");

        if (getResourcesMethod == NULL) {
            DetachCurrentThread();
            return NULL;
        }

        jobject resources = (*env)->CallObjectMethod(env, nativeInstance, getResourcesMethod);

        if (RaymobExceptionCheck(env, "getResources") || resources == NULL) {
            DetachCurrentThread();
            return NULL;
        }

        // Get the getIdentifier method of the Resources class
        jmethodID getIdentifierMethod = RaymobGetMethod(env, resources, "getIdentifier",
                                                        "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)I");

        // Get the package name of the native instance
        jmethodID getPackageNameMethod = RaymobGetMethod(env, nativeInstance, "getPackageName",
                                                         "()Ljava/lang/String;");

        if (getIdentifierMethod == NULL || getPackageNameMethod == NULL) {
            DetachCurrentThread();
            return NULL;
        }

        // Convert string name passed as parameter to jstring
        jstring resourceName = (*env)->NewStringUTF(env, value);
        jstring defType = (*env)->NewStringUTF(env, "string");

        jstring packageName = (jstring)(*env)->CallObjectMethod(env, nativeInstance, getPackageNameMethod);
        RaymobExceptionCheck(env, "getPackageName");

        // Call getIdentifier to get the resource identifier
        jint resId = (*env)->CallIntMethod(env, resources, getIdentifierMethod, resourceName, defType, packageName);

        if (RaymobExceptionCheck(env, "getIdentifier")) resId = 0;

        // Clean up used local references
        (*env)->DeleteLocalRef(env, resourceName);
        (*env)->DeleteLocalRef(env, defType);

        if (resId == 0) {
            // No identifier found for this resource
            DetachCurrentThread();
            return NULL;
        }

        // Call getString with the obtained identifier
        jmethodID getStringMethod = RaymobGetMethod(env, nativeInstance, "getString", "(I)Ljava/lang/String;");

        if (getStringMethod == NULL) {
            DetachCurrentThread();
            return NULL;
        }

        jstring rv = (jstring)(*env)->CallObjectMethod(env, nativeInstance, getStringMethod, resId);

        if (RaymobExceptionCheck(env, "getString") || rv == NULL) {
            DetachCurrentThread();
            return NULL;
        }

        // Convert jstring to char*
        const char* strReturn = (*env)->GetStringUTFChars(env, rv, NULL);

        if (strReturn == NULL) {
            (*env)->DeleteLocalRef(env, rv);
            DetachCurrentThread();
            return NULL;
        }

        // Allocate memory for returned string
        size_t len = strlen(strReturn) + 1;
        char* stringValue = RL_MALLOC(len);

        if (stringValue) {
            strncpy(stringValue, strReturn, len);
            stringValue[len - 1] = '\0'; // Just for security: end with '\0'
        }

        // Free UTF string and clean local references
        (*env)->ReleaseStringUTFChars(env, rv, strReturn);
        (*env)->DeleteLocalRef(env, rv);

        DetachCurrentThread();
        return stringValue;
    }

    return NULL;
}

char* GetAppStoragePath(){

    jobject nativeInstance = GetNativeLoaderInstance();

    if (nativeInstance != NULL)
    {
        JNIEnv* env = AttachCurrentThread();

        if (env == NULL) return NULL;

        // Get the getExternalFilesDir method ID
        jmethodID getExternalFilesDirMethod = RaymobGetMethod(env, nativeInstance, "getExternalFilesDir",
                                                              "(Ljava/lang/String;)Ljava/io/File;");

        if (getExternalFilesDirMethod == NULL)
        {
            DetachCurrentThread();
            return NULL;
        }

        // Call getExternalFilesDir(null) to get the root external files directory
        jobject fileObj = (*env)->CallObjectMethod(env, nativeInstance, getExternalFilesDirMethod, NULL);

        // NOTE: it returns null when the external storage is not mounted
        if (RaymobExceptionCheck(env, "getExternalFilesDir") || fileObj == NULL)
        {
            DetachCurrentThread();
            return NULL;
        }

        // Get the getAbsolutePath() method ID
        jmethodID getAbsolutePathMethod = RaymobGetMethod(env, fileObj, "getAbsolutePath", "()Ljava/lang/String;");

        if (getAbsolutePathMethod == NULL)
        {
            (*env)->DeleteLocalRef(env, fileObj);
            DetachCurrentThread();
            return NULL;
        }

        // Call getAbsolutePath() to get the Java string
        jstring jFilePath = (jstring)(*env)->CallObjectMethod(env, fileObj, getAbsolutePathMethod);

        if (RaymobExceptionCheck(env, "getAbsolutePath") || jFilePath == NULL)
        {
            (*env)->DeleteLocalRef(env, fileObj);
            DetachCurrentThread();
            return NULL;
        }

        // Convert Java string to C string
        const char *cFilePath = (*env)->GetStringUTFChars(env, jFilePath, NULL);

        char *filepath = (cFilePath != NULL) ? strdup(cFilePath) : NULL;

        if (cFilePath != NULL) (*env)->ReleaseStringUTFChars(env, jFilePath, cFilePath);
        (*env)->DeleteLocalRef(env, jFilePath);
        (*env)->DeleteLocalRef(env, fileObj);

        DetachCurrentThread();

        return filepath;
    }

    return NULL;
}

void* ReadFromAppStorage(const char *filepath, int *dataSize){

    char *appStoragePath = GetAppStoragePath();

    if (appStoragePath == NULL) return NULL;

    size_t pathLen = strlen(appStoragePath) + strlen(filepath) + 2;
    char *path = RL_MALLOC(sizeof(char)*pathLen);
    snprintf(path, pathLen, "%s/%s", appStoragePath, filepath);

    unsigned char *data = NULL;
    *dataSize = 0;

    FILE *file = fopen(path, "rb");

    if (file == NULL){
        TraceLog(LOG_WARNING, "FILEIO: [%s] Failed to open file", path);
        RL_FREE(appStoragePath);
        RL_FREE(path);
        return NULL;
    }

    // WARNING: On binary streams SEEK_END could not be found,
    // using fseek() and ftell() could not work in some (rare) cases
    fseek(file, 0, SEEK_END);
    int size = ftell(file);     // WARNING: ftell() returns 'long int', maximum size returned is INT_MAX (2147483647 bytes)
    fseek(file, 0, SEEK_SET);

    if (size > 0)
    {
        data = RL_MALLOC(size*sizeof(unsigned char));

        if (data != NULL)
        {
            // NOTE: fread() returns number of read elements instead of bytes, so we read [1 byte, size elements]
            size_t count = fread(data, sizeof(unsigned char), size, file);

            // WARNING: fread() returns a size_t value, usually 'unsigned int' (32bit compilation) and 'unsigned long long' (64bit compilation)
            // dataSize is unified along raylib as a 'int' type, so, for file-sizes > INT_MAX (2147483647 bytes) we have a limitation
            if (count > 2147483647)
            {
                TraceLog(LOG_WARNING, "FILEIO: [%s] File is bigger than 2147483647 bytes, avoid using LoadFileData()", path);

                RL_FREE(data);
                data = NULL;
            }
            else
            {
                *dataSize = (int)count;

                if ((*dataSize) != size) TraceLog(LOG_WARNING, "FILEIO: [%s] File partially loaded (%i bytes out of %i)", path, *dataSize, size);
                else TraceLog(LOG_INFO, "FILEIO: [%s] File loaded successfully", path);
            }
        }
        else TraceLog(LOG_WARNING, "FILEIO: [%s] Failed to allocated memory for file reading", path);
    }
    else TraceLog(LOG_WARNING, "FILEIO: [%s] Failed to read file", path);

    fclose(file);

    RL_FREE(appStoragePath);
    RL_FREE(path);

    return data;
}

bool WriteToAppStorage(const char *filepath, void *data, unsigned int dataSize){

    char *appStoragePath = GetAppStoragePath();

    if (appStoragePath == NULL) return false;

    size_t pathLen = strlen(appStoragePath) + strlen(filepath) + 2;
    char *path = RL_MALLOC(sizeof(char)*pathLen);
    snprintf(path, pathLen, "%s/%s", appStoragePath, filepath);

    bool success = false;

    FILE *file = fopen(path, "wb");

    if (file != NULL)
    {
        // WARNING: fwrite() returns a size_t value, usually 'unsigned int' (32bit compilation) and 'unsigned long long' (64bit compilation)
        // and expects a size_t input value but as dataSize is limited to INT_MAX (2147483647 bytes), there shouldn't be a problem
        int count = (int)fwrite(data, sizeof(unsigned char), dataSize, file);

        if (count == 0) TraceLog(LOG_WARNING, "FILEIO: [%s] Failed to write file", path);
        else if (count != dataSize) TraceLog(LOG_WARNING, "FILEIO: [%s] File partially written", path);
        else TraceLog(LOG_INFO, "FILEIO: [%s] File saved successfully", path);

        int result = fclose(file);
        if (result == 0) success = true;
    }
    else TraceLog(LOG_WARNING, "FILEIO: [%s] Failed to open file", path);

    RL_FREE(appStoragePath);
    RL_FREE(path);

    return success;
}

bool IsFileExistsInAppStorage(const char *filepath){

    char *appStoragePath = GetAppStoragePath();

    if (appStoragePath == NULL) return false;

    size_t pathLen = strlen(appStoragePath) + strlen(filepath) + 2;
    char *path = RL_MALLOC(sizeof(char)*pathLen);
    snprintf(path, pathLen, "%s/%s", appStoragePath, filepath);

    bool success = (access(path, F_OK) != -1);

    RL_FREE(appStoragePath);
    RL_FREE(path);

    return success;
}

void RemoveFileInAppStorage(const char *filepath){

    char *appStoragePath = GetAppStoragePath();

    if (appStoragePath == NULL) return;

    size_t pathLen = strlen(appStoragePath) + strlen(filepath) + 2;
    char *path = RL_MALLOC(sizeof(char)*pathLen);
    snprintf(path, pathLen, "%s/%s", appStoragePath, filepath);

    remove(path);

    RL_FREE(appStoragePath);
    RL_FREE(path);
}

#endif
