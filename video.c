/* See Copyright Notice in LICENSE.txt */

/*
 * Includes from code by Michael Meeuwisse
 * https://docs.google.com/leaf?id=0B_dz2NwhjXB-NDQ0NWNjOWEtMzJiNy00ZjcwLWJjMjYtZTU2YmQzMWMzYmU0
 *
 * License: 
 * 
 * (C) Copyright 2010 Michael Meeuwisse
 *
 * Adapted from avcodec_sample.0.5.0.c, license unknown
 *
 * ffmpeg_test is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * ffmpeg_test is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with ffmpeg_test. If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <GL/glew.h>
#include <GL/gl.h>
#include <lauxlib.h>
#include <lualib.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>

#include "misc.h"

typedef struct {
    AVFormatContext *format_context;
    AVCodecContext *codec_context;
    AVCodec *codec;
    AVFrame *raw_frame;
    AVFrame *scaled_frame;
    uint8_t *buffer;
    struct SwsContext *scaler;
    int stream_idx, width, height, format;
    GLuint tex;
    double fps;
} video_t;

LUA_TYPE_DECL(video);

/* Helper functions */

static void video_free(video_t *video) {
    if (video->scaler)
        sws_freeContext(video->scaler);
    if (video->raw_frame)
        av_free(video->raw_frame);
    if (video->scaled_frame)
        av_free(video->scaled_frame);

    if (video->codec_context)
        avcodec_close(video->codec_context);
    if (video->format_context)
        av_close_input_file(video->format_context);

    free(video->buffer);
}

static int video_open(video_t *video, const char *filename) {
    video->format = PIX_FMT_RGB24;
    if (av_open_input_file(&video->format_context, filename, NULL, 0, NULL) ||
            av_find_stream_info(video->format_context) < 0) {
        fprintf(stderr, "cannot open video stream %s\n", filename);
        goto failed;
    }

    video->stream_idx = -1;
    for (int i = 0; i < video->format_context->nb_streams; i++) {
        if (video->format_context->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO) {
            video->stream_idx = i;
            break;
        }
    }

    if (video->stream_idx == -1) {
        fprintf(stderr, "cannot find video stream\n");
        goto failed;
    }

    AVStream *stream = video->format_context->streams[video->stream_idx];
    video->codec_context = stream->codec;
    video->codec = avcodec_find_decoder(video->codec_context->codec_id);

    if (!video->codec || avcodec_open(video->codec_context, video->codec) < 0) {
        fprintf(stderr, "cannot open codec\n");
        goto failed;
    }

    /* Save Width/Height */
    video->width = video->codec_context->width;
    video->height = video->codec_context->height;

    /* Frame rate fix for some codecs */
    if (video->codec_context->time_base.num > 1000 && video->codec_context->time_base.den == 1)
        video->codec_context->time_base.den = 1000;

    /* Get FPS */
    // http://libav-users.943685.n4.nabble.com/Retrieving-Frames-Per-Second-FPS-td946533.html
    if ((stream->time_base.den != stream->r_frame_rate.num) ||
            (stream->time_base.num != stream->r_frame_rate.den)) {
        video->fps = 1.0 / stream->r_frame_rate.den * stream->r_frame_rate.num;
    } else {
        video->fps = 1.0 / stream->time_base.num * stream->time_base.den;
    }
    fprintf(stderr, "fps: %lf\n", video->fps);

    /* Get framebuffers */
    video->raw_frame = avcodec_alloc_frame();
    video->scaled_frame = avcodec_alloc_frame();

    if (!video->raw_frame || !video->scaled_frame) {
        fprintf(stderr, "cannot preallocate frames\n");
        goto failed;
    }

    /* Create data buffer */
    video->buffer = malloc(avpicture_get_size(video->format, 
                video->codec_context->width, video->codec_context->height));

    /* Init buffers */
    avpicture_fill(
            (AVPicture *) video->scaled_frame, 
            video->buffer, 
            video->format, 
            video->codec_context->width, 
            video->codec_context->height
            );

    /* Init scale & convert */
    video->scaler = sws_getContext(
            video->codec_context->width, 
            video->codec_context->height, 
            video->codec_context->pix_fmt,
            video->width, 
            video->height, 
            video->format, 
            SWS_BICUBIC, 
            NULL, 
            NULL, 
            NULL
            );

    if (!video->scaler) {
        fprintf(stderr, "scale context init failed\n");
        goto failed;
    }

    /* Give some info on stderr about the file & stream */
    dump_format(video->format_context, 0, filename, 0);
    return 1;
failed:
    video_free(video);
    return 0;
}

static void video_flip(AVFrame* pFrame) {
}

static int video_next_frame(video_t *video) {
    AVPacket packet;
    av_init_packet(&packet);

again:
    /* Can we read a frame? */
    if (av_read_frame(video->format_context, &packet)) {
        fprintf(stderr, "no next frame\n");
        av_free_packet(&packet);
        return 0;
    }

    /* Is it what we're trying to parse? */
    if (packet.stream_index != video->stream_idx) {
        // fprintf(stderr, "not video\n");
        av_free_packet(&packet);
        goto again;
    }

    /* Decode it! */
    int finished = 0;
    avcodec_decode_video2(video->codec_context, video->raw_frame, &finished, &packet);

    /* Success? If not, drop packet. */
    if (!finished) {
        fprintf(stderr, "not complete\n");
        av_free_packet(&packet);
        goto again;
    }

    /* Flip vertically
     * XXX: This feels wrong. What's the right way to do this?
     */
    int heights[] = {
        video->codec_context->height     - 1,
        video->codec_context->height / 2 - 1,
        video->codec_context->height / 2 - 1,
        0,
    };

    for (int i = 0; i < 4; i++) { 
        video->raw_frame->data[i] += video->raw_frame->linesize[i] * heights[i];
        video->raw_frame->linesize[i] = -video->raw_frame->linesize[i]; 
    } 

    sws_scale(
        video->scaler, 
        (const uint8_t* const *)video->raw_frame->data, 
        video->raw_frame->linesize, 
        0, 
        video->codec_context->height, 
        video->scaled_frame->data, 
        video->scaled_frame->linesize
    );
    av_free_packet(&packet);
    return 1;
}

/* Instance methods */

static int video_size(lua_State *L) {
    video_t *video = checked_video(L, 1);
    lua_pushnumber(L, video->width);
    lua_pushnumber(L, video->height);
    return 2;
}

static int video_fps(lua_State *L) {
    video_t *video = checked_video(L, 1);
    lua_pushnumber(L, video->fps);
    return 1;
}

static int video_next(lua_State *L) {
    video_t *video = checked_video(L, 1);

    if (!video_next_frame(video)) {
        lua_pushboolean(L, 0);
        return 1;
    }

    glBindTexture(GL_TEXTURE_2D, video->tex);

    glPixelStorei(GL_UNPACK_SWAP_BYTES, GL_TRUE);
    glPixelStorei(GL_UNPACK_LSB_FIRST,  GL_TRUE);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    glTexSubImage2D(
        GL_TEXTURE_2D,
        0,
        0,
        0,
        video->width,
        video->height,
        GL_RGB,
        GL_UNSIGNED_BYTE,
        video->buffer 
    );
    glGenerateMipmap(GL_TEXTURE_2D);

    lua_pushboolean(L, 1);
    return 1;
}

static int video_draw(lua_State *L) {
    video_t *video = checked_video(L, 1);
    GLfloat x1 = luaL_checknumber(L, 2);
    GLfloat y1 = luaL_checknumber(L, 3);
    GLfloat x2 = luaL_checknumber(L, 4);
    GLfloat y2 = luaL_checknumber(L, 5);
    GLfloat alpha = luaL_optnumber(L, 6, 1.0);

    glBindTexture(GL_TEXTURE_2D, video->tex);
    glColor4f(1.0, 1.0, 1.0, alpha);

    glBegin(GL_QUADS); 
        glTexCoord2f(0.0, 1.0); glVertex3f(x1, y1, 0);
        glTexCoord2f(1.0, 1.0); glVertex3f(x2, y1, 0);
        glTexCoord2f(1.0, 0.0); glVertex3f(x2, y2, 0);
        glTexCoord2f(0.0, 0.0); glVertex3f(x1, y2, 0);
    glEnd();
    return 0;
}

static int video_texid(lua_State *L) {
    video_t *video = checked_video(L, 1);
    lua_pushnumber(L, video->tex);
    return 1;
}

static const luaL_reg video_methods[] = {
    {"draw",    video_draw},
    {"next",    video_next},
    {"size",    video_size},
    {"fps",     video_fps},
    {"texid",   video_texid},
    {0,0}
};

/* Lifecycle */

int video_load(lua_State *L, const char *path, const char *name) {
    video_t video;
    memset(&video, 0, sizeof(video_t));

    if (!video_open(&video, path))
        luaL_error(L, "cannot open video %s", name);

    glGenTextures(1, &video.tex);
    glBindTexture(GL_TEXTURE_2D, video.tex);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexImage2D(
        GL_TEXTURE_2D,  
        0,
        GL_RGB, 
        video.width,
        video.height,
        0,
        GL_RGB,
        GL_UNSIGNED_BYTE,
        NULL 
    );

    *push_video(L) = video;
    return 1;
}

static int video_gc(lua_State *L) {
    video_t *video = to_video(L, 1);
    fprintf(stderr, "gc'ing video: tex id: %d\n", video->tex);
    glDeleteTextures(1, &video->tex);
    video_free(video);
    return 0;
}

LUA_TYPE_IMPL(video);
