/*
 * egl-dmabuf-probe: exercise EGL_LINUX_DMA_BUF_EXT image creation on a
 * virpipe (vtest) EGL display.
 *
 * First POC step for the "last cell": before the Mesa patch, the vtest
 * winsys leaves resource_create_from_handle() NULL, so this call either
 * crashes or fails inside Mesa.  After the patch, the image is created as a
 * regular virgl texture and this program prints PASS.
 *
 * Must run inside a guest session that has VTEST_SOCKET_NAME pointing at the
 * host's vtest socket, with:
 *   LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=virpipe
 */
#define _GNU_SOURCE
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <gbm.h>

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* DRM_FORMAT_ABGR8888 = fourcc('A', 'B', '2', '4').  Kept local so the probe
 * does not require libdrm headers just for one format. */
#ifndef DRM_FORMAT_ABGR8888
#define DRM_FORMAT_ABGR8888 0x34324241
#endif

static void fail(const char *what) {
    fprintf(stderr, "%s failed (EGL error 0x%x)\n", what, eglGetError());
    exit(1);
}

int main(int argc, char **argv) {
    const char *node = argc > 1 ? argv[1] : "/dev/dri/renderD128";
    int fd = open(node, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        perror(node);
        return 2;
    }

    struct gbm_device *gbm = gbm_create_device(fd);
    if (!gbm) {
        fprintf(stderr, "gbm_create_device(%s) failed\n", node);
        return 2;
    }

    EGLDisplay dpy = eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, gbm, NULL);
    if (dpy == EGL_NO_DISPLAY) {
        fprintf(stderr, "eglGetPlatformDisplay(GBM) failed\n");
        return 2;
    }

    EGLint major = 0, minor = 0;
    if (!eglInitialize(dpy, &major, &minor))
        fail("eglInitialize");

    const char *exts = eglQueryString(dpy, EGL_EXTENSIONS);
    int has_import = exts && strstr(exts, "EGL_EXT_image_dma_buf_import") != NULL;
    printf("EGL %d.%d\n", major, minor);
    printf("EGL_EXT_image_dma_buf_import=%d\n", has_import);
    if (!has_import) {
        fprintf(stderr, "EGL_EXT_image_dma_buf_import is not advertised\n");
        return 3;
    }

    eglBindAPI(EGL_OPENGL_ES_API);
    const int no_config = exts && strstr(exts, "EGL_KHR_no_config_context") != NULL;
    const int surfaceless = exts && strstr(exts, "EGL_KHR_surfaceless_context") != NULL;

    EGLint cfg_attr[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_NONE,
    };
    EGLConfig config = NULL;
    EGLint count = 0;
    const int have_config =
        eglChooseConfig(dpy, cfg_attr, &config, 1, &count) && count > 0;

    /* A minimal Mesa build (or a GBM-only EGL) may expose no pbuffer
     * configurations.  The image import test only needs a current context,
     * so fall back to EGL_KHR_no_config_context + surfaceless. */
    if (!have_config && no_config)
        config = (EGLConfig)0; /* EGL_NO_CONFIG_KHR */

    EGLSurface surface = EGL_NO_SURFACE;
    if (have_config && !surfaceless) {
        EGLint pb_attr[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
        surface = eglCreatePbufferSurface(dpy, config, pb_attr);
        if (surface == EGL_NO_SURFACE)
            fail("eglCreatePbufferSurface");
    }

    EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext context = eglCreateContext(dpy, config, EGL_NO_CONTEXT, ctx_attr);
    if (context == EGL_NO_CONTEXT)
        fail("eglCreateContext");
    if (!eglMakeCurrent(dpy, surface, surface, context))
        fail("eglMakeCurrent");

    const char *renderer = (const char *)glGetString(GL_RENDERER);
    printf("GL_RENDERER=%s\n", renderer ? renderer : "(null)");
    if (!renderer || !strstr(renderer, "virgl")) {
        fprintf(stderr, "not a virgl renderer; set LIBGL_ALWAYS_SOFTWARE=1 "
                        "GALLIUM_DRIVER=virpipe\n");
        return 4;
    }

    const int width = 256, height = 256;
    int dmabuf_fd = -1;
    uint32_t stride = (uint32_t)width * 4;

    struct gbm_bo *bo = gbm_bo_create(gbm, width, height, GBM_FORMAT_ABGR8888,
                                      GBM_BO_USE_RENDERING);
    if (bo) {
        dmabuf_fd = gbm_bo_get_fd(bo);
        stride = gbm_bo_get_stride(bo);
    }
    if (dmabuf_fd < 0) {
        /* The import hook creates a regular virgl texture and only uses the
         * fd as an identity token, so a memfd is enough to exercise it when
         * the local GBM backend cannot allocate. */
        fprintf(stderr, "gbm_bo_create/get_fd failed; using a memfd token\n");
        dmabuf_fd = memfd_create("tvg-token", 0);
        if (dmabuf_fd < 0 || ftruncate(dmabuf_fd, (off_t)stride * height) < 0) {
            perror("memfd token");
            return 2;
        }
    }

    PFNEGLCREATEIMAGEKHRPROC create_image =
        (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_target =
        (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress(
            "glEGLImageTargetTexture2DOES");
    if (!create_image || !image_target) {
        fprintf(stderr, "missing EGLImage entry points\n");
        return 2;
    }

    EGLint image_attr[] = {
        EGL_WIDTH, width,
        EGL_HEIGHT, height,
        EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_ABGR8888,
        EGL_DMA_BUF_PLANE0_FD_EXT, dmabuf_fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
        EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)stride,
        EGL_NONE,
    };
    EGLImage image = create_image(dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT,
                                  NULL, image_attr);
    if (image == EGL_NO_IMAGE_KHR) {
        fprintf(stderr, "eglCreateImageKHR failed (EGL error 0x%x)\n",
                eglGetError());
        return 5;
    }

    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    image_target(GL_TEXTURE_2D, image);
    GLenum error = glGetError();
    glFinish();

    printf("eglCreateImageKHR=OK glEGLImageTargetTexture2DOES=0x%x\n", error);
    printf("%s\n", error == GL_NO_ERROR ? "PASS" : "FAIL");
    /* Keep the vtest connection (and its resources) alive long enough for the
     * host token service's present request to be processed. */
    sleep(2);
    return error == GL_NO_ERROR ? 0 : 6;
}
