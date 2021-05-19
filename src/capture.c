#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <getopt.h>
#include <time.h>

#include <pthread.h>

#include <GLES2/gl2.h>
#include <EGL/egl.h>
#include <vt/vt_openapi.h>

#include "debug.h"
#include "hyperion_client.h"

#define MAX(a, b) (((a) > (b)) ? (a) : (b))

static struct option long_options[] = {
    {"width", optional_argument, 0, 'x'},
    {"height", optional_argument, 0, 'y'},
    {"address", required_argument, 0, 'a'},
    {"port", optional_argument, 0, 'p'},
    {"fps", optional_argument, 0, 'f'},
    {0, 0, 0, 0},
};

EGLDisplay egl_display;
EGLContext egl_context;
EGLSurface egl_surface;

pthread_mutex_t frame_mutex = PTHREAD_MUTEX_INITIALIZER;

VT_VIDEO_WINDOW_ID window_id;
VT_RESOURCE_ID resource_id;
VT_CONTEXT_ID context_id;
GLuint texture_id = 0;
GLuint offscreen_fb = 0;

GLubyte *pixels_rgba = NULL, *pixels_rgb = NULL;

bool app_quit = false;
bool capture_initialized = false;
bool vt_available = false;

VT_RESOLUTION_T resolution = {192, 108};
static const char *_address = NULL;
static int _port = 19400, _fps = 15;

void egl_init();
void egl_cleanup();

int capture_initialize();
void capture_terminate();
void capture_onevent(VT_EVENT_TYPE_T type, void *data, void *user_data);
void send_picture();

static void print_usage()
{
    printf("Usage: hyperion-webos -a ADDRESS [OPTION]...\n");
    printf("\n");
    printf("Grab screen content continously and send to Hyperion via flatbuffers server.\n");
    printf("\n");
    printf("  -x, --width           Width of video frame (default 192)\n");
    printf("  -y, --height          Height of video frame (default 108)\n");
    printf("  -a, --address         IP address of Hyperion server\n");
    printf("  -p, --port            Port of Hyperion flatbuffers server (default 19400)\n");
    printf("  -f, --fps             Framerate for sending video frames (default 15)\n");
}

static int parse_options(int argc, char *argv[])
{
    int opt, longindex;
    while ((opt = getopt_long(argc, argv, "x:y:a:p:f:", long_options, &longindex)) != -1)
    {
        switch (opt)
        {
        case 'x':
            resolution.w = atoi(optarg);
            break;
        case 'y':
            resolution.h = atoi(optarg);
            break;
        case 'a':
            _address = strdup(optarg);
            break;
        case 'p':
            _port = atol(optarg);
            break;
        case 'f':
            _fps = atoi(optarg);
            break;
        }
    }
    if (!_address)
    {
        fprintf(stderr, "Error! Address not specified.\n");
        print_usage();
        return 1;
    }
    if (_fps < 1 || _fps > 60)
    {
        fprintf(stderr, "Error! FPS should between 1 and 60.\n");
        print_usage();
        return 1;
    }
    return 0;
}

int main(int argc, char *argv[])
{
    int ret;
    if ((ret = parse_options(argc, argv)) != 0)
    {
        return ret;
    }
    if (getenv("XDG_RUNTIME_DIR") == NULL)
    {
        setenv("XDG_RUNTIME_DIR", "/tmp/xdg", 1);
    }

    egl_init();

    if ((ret = capture_initialize()) != 0)
    {
        return ret;
    }
    pixels_rgba = (GLubyte *)calloc(resolution.w * resolution.h, 4 * sizeof(GLubyte));
    pixels_rgb = (GLubyte *)calloc(resolution.w * resolution.h, 3 * sizeof(GLubyte));
    hyperion_client("webos", _address, _port, 150);
    while (!app_quit)
    {
        if (hyperion_read() < 0)
        {
            fprintf(stderr, "Connection terminated.\n");
            app_quit = true;
        }
    }

    hyperion_destroy();
    capture_terminate();
    egl_cleanup();
    return 0;
}

void egl_init()
{
    // 1. Initialize egl
    egl_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    assert(eglGetError() == EGL_SUCCESS);
    assert(egl_display);
    EGLint major, minor;

    eglInitialize(egl_display, &major, &minor);
    assert(eglGetError() == EGL_SUCCESS);
    printf("EGL Display, major = %d, minor = %d\n", major, minor);

    // 2. Select an appropriate configuration
    EGLint numConfigs;
    EGLConfig eglCfg;

    const EGLint configAttribs[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_BLUE_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_RED_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_NONE};

    eglChooseConfig(egl_display, configAttribs, &eglCfg, 1, &numConfigs);
    assert(eglGetError() == EGL_SUCCESS);

    // 3. Create a surface

    EGLint pbufferAttribs[] = {
        EGL_WIDTH, resolution.w,
        EGL_HEIGHT, resolution.h,
        EGL_TEXTURE_FORMAT, EGL_TEXTURE_RGBA,
        EGL_TEXTURE_TARGET, EGL_TEXTURE_2D,
        EGL_LARGEST_PBUFFER, EGL_TRUE,
        EGL_NONE};
    egl_surface = eglCreatePbufferSurface(egl_display, eglCfg, pbufferAttribs);
    assert(eglGetError() == EGL_SUCCESS);
    assert(egl_surface);

    // 4. Bind the API
    eglBindAPI(EGL_OPENGL_ES_API);
    assert(eglGetError() == EGL_SUCCESS);

    // 5. Create a context and make it current

    EGLint contextAttribs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE};
    egl_context = eglCreateContext(egl_display, eglCfg, EGL_NO_CONTEXT, contextAttribs);
    assert(eglGetError() == EGL_SUCCESS);
    assert(egl_context);

    eglMakeCurrent(egl_display, egl_surface, egl_surface, egl_context);
    assert(eglGetError() == EGL_SUCCESS);

    EGLint suf_width, suf_height;
    eglQuerySurface(egl_display, egl_surface, EGL_WIDTH, &suf_width);
    eglQuerySurface(egl_display, egl_surface, EGL_HEIGHT, &suf_height);
    assert(eglGetError() == EGL_SUCCESS);
    printf("EGL Surface size: %dx%d\n", suf_width, suf_height);

    // Create framebuffer for offscreen rendering
    GL_CHECK(glGenFramebuffers(1, &offscreen_fb));
    GL_CHECK(glBindFramebuffer(GL_FRAMEBUFFER, offscreen_fb));

    printf("EGL init complete");
}

void egl_cleanup()
{
    glDeleteFramebuffers(1, &offscreen_fb);
    eglDestroyContext(egl_display, egl_context);
    eglDestroySurface(egl_display, egl_surface);
    eglTerminate(egl_display);
    free(pixels_rgb);
    free(pixels_rgba);
}

int capture_initialize()
{
    window_id = VT_CreateVideoWindow(0);
    if (window_id == -1)
    {
        fprintf(stderr, "[Capture Sample] VT_CreateVideoWindow Failed\n");
        return -1;
    }
    fprintf(stderr, "[Capture Sample] window_id=%d\n", window_id);

    if (VT_AcquireVideoWindowResource(window_id, &resource_id) != VT_OK)
    {
        fprintf(stderr, "[Capture Sample] VT_AcquireVideoWindowResource Failed\n");
        return -1;
    }
    fprintf(stderr, "[Capture Sample] resource_id=%d\n", resource_id);

    context_id = VT_CreateContext(resource_id, 2);
    if (!context_id || context_id == -1)
    {
        fprintf(stderr, "[Capture Sample] VT_CreateContext Failed\n");
        VT_ReleaseVideoWindowResource(resource_id);
        return -1;
    }
    fprintf(stderr, "[Capture Sample] context_id=%d\n", context_id);

    VT_SetTextureResolution(context_id, &resolution);
    // VT_GetTextureResolution(context_id, &resolution);

    if (VT_SetTextureSourceRegion(context_id, VT_SOURCE_REGION_MAX) != VT_OK)
    {
        fprintf(stderr, "[Capture Sample] VT_SetTextureSourceRegion Failed\n");
        VT_DeleteContext(context_id);
        VT_ReleaseVideoWindowResource(resource_id);
        return -1;
    }

    if (VT_SetTextureSourceLocation(context_id, VT_SOURCE_LOCATION_DISPLAY) != VT_OK)
    {
        fprintf(stderr, "[Capture Sample] VT_SetTextureSourceLocation Failed\n");
        VT_DeleteContext(context_id);
        VT_ReleaseVideoWindowResource(resource_id);
        return -1;
    }

    if (VT_RegisterEventHandler(context_id, &capture_onevent, NULL) != VT_OK)
    {
        fprintf(stderr, "[Capture Sample] VT_RegisterEventHandler Failed\n");
        VT_DeleteContext(context_id);
        VT_ReleaseVideoWindowResource(resource_id);
        return -1;
    }
    capture_initialized = true;
    return 0;
}

void capture_terminate()
{
    if (!capture_initialized)
        return;
    capture_initialized = false;

    if (texture_id != 0 && glIsTexture(texture_id))
    {
        VT_DeleteTexture(context_id, texture_id);
    }

    if (VT_UnRegisterEventHandler(context_id) != VT_OK)
    {
        fprintf(stderr, "[Capture Sample] VT_UnRegisterEventHandler error!\n");
    }
    VT_DeleteContext(context_id);
    VT_ReleaseVideoWindowResource(resource_id);
}

uint32_t getticks()
{
    struct timespec tp;
    clock_gettime(CLOCK_MONOTONIC, &tp);
    return tp.tv_sec * 1000 + tp.tv_nsec / 1000000;
}

void capture_acquire()
{
    pthread_mutex_lock(&frame_mutex);
    static uint32_t fps_ticks = 0, last_send_ticks = 0, framecount = 0;
    VT_OUTPUT_INFO_T output_info;
    if (vt_available)
    {
        if (capture_initialized)
        {
            if (texture_id != 0 && glIsTexture(texture_id))
            {
                VT_DeleteTexture(context_id, texture_id);
            }

            VT_STATUS_T vtStatus = VT_GenerateTexture(resource_id, context_id, &texture_id, &output_info);
            if (vtStatus == VT_OK)
            {
                if (last_send_ticks == 0 || (getticks() - last_send_ticks) >= MAX(0, (1000 / _fps) - 16))
                {
                    send_picture();
                    uint32_t end_ticks = getticks();
                    last_send_ticks = end_ticks;
                    if ((end_ticks - fps_ticks) >= 1000)
                    {
                        printf("Capture speed %d FPS (requested %d)\n", (int)(framecount * 1000.0 / (end_ticks - fps_ticks)), _fps);
                        fps_ticks = end_ticks;
                        framecount = 0;
                    }
                    else
                    {
                        framecount++;
                    }
                }
            }
            else
            {
                fprintf(stderr, "VT_GenerateTexture failed\n");
                texture_id = 0;
            }
        }
        vt_available = false;
    }
    pthread_mutex_unlock(&frame_mutex);
}

void capture_onevent(VT_EVENT_TYPE_T type, void *data, void *user_data)
{
    switch (type)
    {
    case VT_AVAILABLE:
        vt_available = true;
        capture_acquire();
        break;
    case VT_UNAVAILABLE:
        fprintf(stderr, "VT_UNAVAILABLE received\n");
        break;
    case VT_RESOURCE_BUSY:
        fprintf(stderr, "VT_RESOURCE_BUSY received\n");
        break;
    default:
        fprintf(stderr, "UNKNOWN event received\n");
        break;
    }
}

void send_picture()
{
    int width = resolution.w, height = resolution.h;

    glBindFramebuffer(GL_FRAMEBUFFER, offscreen_fb);

    glBindTexture(GL_TEXTURE_2D, texture_id);

    //Bind the texture to your FBO
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture_id, 0);

    //Test if everything failed
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE)
    {
        fprintf(stderr, "failed to make complete framebuffer object %x", status);
    }

    glViewport(0, 0, width, height);

    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels_rgba);
    for (int y = 0; y < height; y++)
    {
        for (int x = 0; x < width; x++)
        {
            int i1 = (y * width) + x, i2 = ((height - y - 1) * width) + x;
            pixels_rgb[i1 * 3 + 0] = pixels_rgba[i2 * 4 + 0];
            pixels_rgb[i1 * 3 + 1] = pixels_rgba[i2 * 4 + 1];
            pixels_rgb[i1 * 3 + 2] = pixels_rgba[i2 * 4 + 2];
        }
    }
    if (hyperion_set_image(pixels_rgb, width, height) != 0)
    {
        fprintf(stderr, "Write timeout\n");
        hyperion_destroy();
        app_quit = true;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}