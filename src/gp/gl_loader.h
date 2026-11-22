// SPDX-License-Identifier: MIT
// Minimal OpenGL 3.3 core loader: types, constants and entry points used by the GP renderer, resolved at
// run time through SDL_GL_GetProcAddress so no system libGL/opengl32 symbol is linked directly.
// The declarations follow the Khronos OpenGL registry (glcorearb.h, MIT/Apache-2.0).
// Generated for the functions the renderer uses; add a line to GCRECOMP_GL_FUNCTIONS to use a new one.
#pragma once

#include <cstddef>
#include <cstdint>

#if defined(_WIN32)
#define GCRECOMP_GLAPI __stdcall
#else
#define GCRECOMP_GLAPI
#endif

typedef unsigned int GLbitfield;
typedef unsigned char GLboolean;
typedef char GLchar;
typedef double GLdouble;
typedef unsigned int GLenum;
typedef float GLfloat;
typedef int GLint;
typedef int GLsizei;
typedef std::ptrdiff_t GLsizeiptr;
typedef std::uint8_t GLubyte;
typedef unsigned int GLuint;

#define GL_ALWAYS 0x0207
#define GL_ARRAY_BUFFER 0x8892
#define GL_BACK 0x0405
#define GL_BLEND 0x0BE2
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_CLEAR 0x1500
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_COLOR_BUFFER_BIT 0x00004000
#define GL_COLOR_LOGIC_OP 0x0BF2
#define GL_COMPILE_STATUS 0x8B81
#define GL_CULL_FACE 0x0B44
#define GL_DEPTH_ATTACHMENT 0x8D00
#define GL_DEPTH_BUFFER_BIT 0x00000100
#define GL_DEPTH_COMPONENT 0x1902
#define GL_DEPTH_COMPONENT24 0x81A6
#define GL_DEPTH_TEST 0x0B71
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_DST_ALPHA 0x0304
#define GL_DST_COLOR 0x0306
#define GL_EQUAL 0x0202
#define GL_FALSE 0
#define GL_FLOAT 0x1406
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_FRAMEBUFFER 0x8D40
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_FRONT 0x0404
#define GL_FRONT_AND_BACK 0x0408
#define GL_FUNC_ADD 0x8006
#define GL_FUNC_REVERSE_SUBTRACT 0x800B
#define GL_GEQUAL 0x0206
#define GL_GREATER 0x0204
#define GL_LEQUAL 0x0203
#define GL_LESS 0x0201
#define GL_LINEAR 0x2601
#define GL_LINEAR_MIPMAP_LINEAR 0x2703
#define GL_LINEAR_MIPMAP_NEAREST 0x2701
#define GL_LINES 0x0001
#define GL_LINE_STRIP 0x0003
#define GL_LINK_STATUS 0x8B82
#define GL_MIRRORED_REPEAT 0x8370
#define GL_NEAREST 0x2600
#define GL_NEAREST_MIPMAP_LINEAR 0x2702
#define GL_NEAREST_MIPMAP_NEAREST 0x2700
#define GL_NEVER 0x0200
#define GL_NOTEQUAL 0x0205
#define GL_NO_ERROR 0
#define GL_ONE 1
#define GL_ONE_MINUS_DST_ALPHA 0x0305
#define GL_ONE_MINUS_DST_COLOR 0x0307
#define GL_ONE_MINUS_SRC1_COLOR 0x88FA
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_ONE_MINUS_SRC_COLOR 0x0301
#define GL_PACK_ALIGNMENT 0x0D05
#define GL_POINTS 0x0000
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_RENDERBUFFER 0x8D41
#define GL_RENDERER 0x1F01
#define GL_REPEAT 0x2901
#define GL_RGB 0x1907
#define GL_RGBA 0x1908
#define GL_RGBA8 0x8058
#define GL_SCISSOR_TEST 0x0C11
#define GL_SRC1_COLOR 0x88F9
#define GL_SRC_ALPHA 0x0302
#define GL_SRC_COLOR 0x0300
#define GL_STREAM_DRAW 0x88E0
#define GL_TEXTURE0 0x84C0
#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE_BASE_LEVEL 0x813C
#define GL_TEXTURE_LOD_BIAS 0x8501
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_MAX_LEVEL 0x813D
#define GL_TEXTURE_MAX_LOD 0x813B
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_MIN_LOD 0x813A
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_TRIANGLES 0x0004
#define GL_TRIANGLE_FAN 0x0006
#define GL_TRIANGLE_STRIP 0x0005
#define GL_TRUE 1
#define GL_UNPACK_ALIGNMENT 0x0CF5
#define GL_UNSIGNED_BYTE 0x1401
#define GL_UNSIGNED_INT 0x1405
#define GL_VERSION 0x1F02
#define GL_VERTEX_SHADER 0x8B31
#define GL_ZERO 0

// X(ReturnType, name, (parameters))
#define GCRECOMP_GL_FUNCTIONS(X) \
    X(void, glActiveTexture, (GLenum texture)) \
    X(void, glAttachShader, (GLuint program, GLuint shader)) \
    X(void, glBindBuffer, (GLenum target, GLuint buffer)) \
    X(void, glBindFramebuffer, (GLenum target, GLuint framebuffer)) \
    X(void, glBindRenderbuffer, (GLenum target, GLuint renderbuffer)) \
    X(void, glBindTexture, (GLenum target, GLuint texture)) \
    X(void, glBindVertexArray, (GLuint array)) \
    X(void, glBlendEquationSeparate, (GLenum modeRGB, GLenum modeAlpha)) \
    X(void, glBlendFunc, (GLenum sfactor, GLenum dfactor)) \
    X(void, glBlendFuncSeparate, (GLenum sfactorRGB, GLenum dfactorRGB, GLenum sfactorAlpha, GLenum dfactorAlpha)) \
    X(void, glBlitFramebuffer, (GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1, GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1, GLbitfield mask, GLenum filter)) \
    X(void, glBufferData, (GLenum target, GLsizeiptr size, const void *data, GLenum usage)) \
    X(GLenum, glCheckFramebufferStatus, (GLenum target)) \
    X(void, glClear, (GLbitfield mask)) \
    X(void, glClearColor, (GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha)) \
    X(void, glClearDepth, (GLdouble depth)) \
    X(void, glColorMask, (GLboolean red, GLboolean green, GLboolean blue, GLboolean alpha)) \
    X(void, glCompileShader, (GLuint shader)) \
    X(GLuint, glCreateProgram, (void)) \
    X(GLuint, glCreateShader, (GLenum type)) \
    X(void, glCullFace, (GLenum mode)) \
    X(void, glDepthFunc, (GLenum func)) \
    X(void, glDepthMask, (GLboolean flag)) \
    X(void, glDisable, (GLenum cap)) \
    X(void, glDrawArrays, (GLenum mode, GLint first, GLsizei count)) \
    X(void, glEnable, (GLenum cap)) \
    X(void, glEnableVertexAttribArray, (GLuint index)) \
    X(void, glFramebufferRenderbuffer, (GLenum target, GLenum attachment, GLenum renderbuffertarget, GLuint renderbuffer)) \
    X(void, glFramebufferTexture2D, (GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level)) \
    X(void, glGenBuffers, (GLsizei n, GLuint *buffers)) \
    X(void, glGenFramebuffers, (GLsizei n, GLuint *framebuffers)) \
    X(void, glGenRenderbuffers, (GLsizei n, GLuint *renderbuffers)) \
    X(void, glGenTextures, (GLsizei n, GLuint *textures)) \
    X(void, glGenVertexArrays, (GLsizei n, GLuint *arrays)) \
    X(GLenum, glGetError, (void)) \
    X(void, glGetProgramInfoLog, (GLuint program, GLsizei bufSize, GLsizei *length, GLchar *infoLog)) \
    X(void, glGetProgramiv, (GLuint program, GLenum pname, GLint *params)) \
    X(void, glGetShaderInfoLog, (GLuint shader, GLsizei bufSize, GLsizei *length, GLchar *infoLog)) \
    X(void, glGetShaderiv, (GLuint shader, GLenum pname, GLint *params)) \
    X(const GLubyte *, glGetString, (GLenum name)) \
    X(GLint, glGetUniformLocation, (GLuint program, const GLchar *name)) \
    X(void, glLinkProgram, (GLuint program)) \
    X(void, glLogicOp, (GLenum opcode)) \
    X(void, glPixelStorei, (GLenum pname, GLint param)) \
    X(void, glReadBuffer, (GLenum src)) \
    X(void, glReadPixels, (GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void *pixels)) \
    X(void, glRenderbufferStorage, (GLenum target, GLenum internalformat, GLsizei width, GLsizei height)) \
    X(void, glScissor, (GLint x, GLint y, GLsizei width, GLsizei height)) \
    X(void, glShaderSource, (GLuint shader, GLsizei count, const GLchar *const*string, const GLint *length)) \
    X(void, glTexImage2D, (GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const void *pixels)) \
    X(void, glTexParameterf, (GLenum target, GLenum pname, GLfloat param)) \
    X(void, glTexParameteri, (GLenum target, GLenum pname, GLint param)) \
    X(void, glUniform1f, (GLint location, GLfloat v0)) \
    X(void, glUniform1i, (GLint location, GLint v0)) \
    X(void, glUniform2ui, (GLint location, GLuint v0, GLuint v1)) \
    X(void, glUniform3fv, (GLint location, GLsizei count, const GLfloat *value)) \
    X(void, glUniform4f, (GLint location, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3)) \
    X(void, glUniform4fv, (GLint location, GLsizei count, const GLfloat *value)) \
    X(void, glUniform4i, (GLint location, GLint v0, GLint v1, GLint v2, GLint v3)) \
    X(void, glUniform4iv, (GLint location, GLsizei count, const GLint *value)) \
    X(void, glUniform4uiv, (GLint location, GLsizei count, const GLuint *value)) \
    X(void, glUseProgram, (GLuint program)) \
    X(void, glVertexAttribPointer, (GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void *pointer)) \
    X(void, glViewport, (GLint x, GLint y, GLsizei width, GLsizei height)) \

#define GCRECOMP_GL_DECLARE(ret, name, params) extern "C++" ret(GCRECOMP_GLAPI* name) params;
GCRECOMP_GL_FUNCTIONS(GCRECOMP_GL_DECLARE)

namespace gcrecomp::gp {
// Resolves every entry point for the current context; returns false (and logs the first missing name) on failure.
bool gl_load_functions();
}  // namespace gcrecomp::gp
