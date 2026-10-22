"""Exercise the existing GLES-compatible VAO cache with a stateful GL model."""
import argparse
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument("--wsl")
parser.add_argument("--web", action="store_true")
args = parser.parse_args()
root = Path(__file__).resolve().parent.parent
source = (root / "port/linux/src/d3d8_gl.c").read_text()

def function(signature):
    start = source.index(signature)
    opening = source.index("{", start)
    level = 1
    end = opening + 1
    while level:
        level += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]

pointer_struct = source[source.index("struct attribute_pointer\n"):source.index("#else\n/* desktop GL")]
cache = source[source.index("#define VERTEX_ARRAY_CACHE_SLOTS"):source.index("\n#endif\n\nstatic void setup_streams")]
program = ("#define HALO_WEB 1\n" if args.web else "#define HALO_ANDROID 1\n") + r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef unsigned int GLuint, GLenum;
typedef int GLint, GLsizei, BOOL;
typedef unsigned char GLboolean;
#define TRUE 1
#define FALSE 0
#define GL_TRUE 1
#define GL_FALSE 0
#define GL_ARRAY_BUFFER 0x8892
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#define GL_UNSIGNED_INT 0x1405
#define GL_FLOAT 0x1406
#define XGPU_VERTEX_ATTRIBUTE_COUNT 16
#define D3DVSDT_NONE 0
#define D3DVSDT_NORMPACKED3 1
#define D3DVSDT_FLOAT4 2
struct vertex_element { unsigned stream, type, reg, offset; };
struct vertex_shader_object { struct vertex_element elements[16]; unsigned long element_count; };
static struct { GLuint vertex_array; struct { unsigned long stride; } streams[16]; } device = { .vertex_array = 1 };
''' + pointer_struct + r'''
static struct { GLuint array_buffer, element_array_buffer; unsigned char attribute_enabled[16]; struct attribute_pointer attribute_pointers[16]; } gl_state;
static struct { GLuint element; unsigned enabled; struct attribute_pointer attrs[16]; int deleted; } actual[12000];
static GLuint current_vao = 1, current_array_buffer, next_vao = 2;
static unsigned pointers, ipointers, binds, element_binds, deletions;
static int allocation_failure, vao_failure;
static void *test_calloc(size_t count, size_t size) { return allocation_failure ? NULL : calloc(count, size); }
static void glBindBuffer(GLenum target, GLuint buffer) {
    if (target == GL_ARRAY_BUFFER) current_array_buffer = buffer;
    else { assert(target == GL_ELEMENT_ARRAY_BUFFER); actual[current_vao].element = buffer; element_binds++; }
}
static void glEnableVertexAttribArray(GLuint index) { actual[current_vao].enabled |= 1U << index; }
static void glVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void *offset) {
    pointers++;
    actual[current_vao].attrs[index] = (struct attribute_pointer){current_array_buffer, size, type, normalized, GL_FALSE, stride, (unsigned long)offset};
}
static void glVertexAttribIPointer(GLuint index, GLint size, GLenum type, GLsizei stride, const void *offset) {
    ipointers++;
    actual[current_vao].attrs[index] = (struct attribute_pointer){current_array_buffer, size, type, GL_FALSE, GL_TRUE, stride, (unsigned long)offset};
}
static void glGenVertexArrays(GLsizei count, GLuint *arrays) {
    for (int i=0; i<count; i++) { assert(next_vao < 12000); arrays[i] = vao_failure ? 0 : next_vao++; }
}
static void glBindVertexArray(GLuint array) { assert(array < next_vao && !actual[array].deleted); current_vao = array; binds++; }
#ifdef HALO_WEB
static void glDeleteVertexArrays(GLsizei count, const GLuint *arrays) {
    for (int i=0; i<count; i++) { assert(arrays[i] != current_vao); actual[arrays[i]].deleted = 1; deletions++; }
}
#endif
static void attribute_format(const struct vertex_element *element, GLint *size, GLenum *type, GLboolean *normalized) {
    assert(element->type == D3DVSDT_FLOAT4); *size = 4; *type = GL_FLOAT; *normalized = GL_FALSE;
}
''' + function("static void state_array_buffer(") + function("static void state_element_array_buffer(") + function("static void state_attribute_stream(") + r'''
#define calloc test_calloc
''' + cache + r'''
#undef calloc
static void invalidate(void) { memset(&gl_state, 0xff, sizeof(gl_state)); vertex_array_cache_invalidate(); }
static void baseline(const struct vertex_shader_object *declaration, const GLuint *buffers, const unsigned long *offsets) {
    for (unsigned i=0; i<declaration->element_count; i++) {
        const struct vertex_element *element = &declaration->elements[i];
        int integer = element->type == D3DVSDT_NORMPACKED3;
        state_attribute_stream(element->reg, element->stream, buffers[element->stream], integer ? 1 : 4,
            integer ? GL_UNSIGNED_INT : GL_FLOAT, GL_FALSE, integer, (GLsizei)device.streams[element->stream].stride,
            offsets[element->stream], element->offset);
    }
}
static void validate(const struct vertex_shader_object *declaration, const GLuint *buffers, const unsigned long *offsets) {
    for (unsigned i=0; i<declaration->element_count; i++) {
        const struct vertex_element *element = &declaration->elements[i];
        const struct attribute_pointer *a = &actual[current_vao].attrs[element->reg];
        assert(actual[current_vao].enabled & (1U << element->reg));
        assert(a->buffer == buffers[element->stream]);
        assert(a->offset == offsets[element->stream] + element->offset);
        assert(a->stride == (GLsizei)device.streams[element->stream].stride);
        assert(a->integer == (element->type == D3DVSDT_NORMPACKED3));
        assert(a->size == (a->integer ? 1 : 4));
    }
}
int main(void) {
    struct vertex_shader_object declaration = {0}, other;
    GLuint buffers[16] = {101, 102}, saved[400];
    unsigned long offsets[16] = {0};
    unsigned old_calls, old_binds, old_elements;
    declaration.element_count = 6;
    for (unsigned i=0; i<6; i++) declaration.elements[i] = (struct vertex_element){i%2, i%2 ? D3DVSDT_NORMPACKED3 : D3DVSDT_FLOAT4, i, (i/2)*4};
    device.streams[0].stride = 32; device.streams[1].stride = 16;
    invalidate();
    for (unsigned frame=0; frame<2; frame++) for (unsigned draw=0; draw<400; draw++) {
        offsets[0] = draw*64; offsets[1] = draw*32;
        baseline(&declaration, buffers, offsets); validate(&declaration, buffers, offsets);
    }
    assert(pointers + ipointers == 4800);
    old_calls = pointers + ipointers;
    for (unsigned frame=0; frame<2; frame++) {
        invalidate();
        for (unsigned draw=0; draw<400; draw++) {
            offsets[0] = draw*64; offsets[1] = draw*32;
            assert(vertex_array_cache_use(&declaration, 3, buffers, offsets));
            if (!frame) saved[draw] = current_vao; else assert(saved[draw] == current_vao);
            state_element_array_buffer(200 + draw%8);
            assert(actual[current_vao].element == 200 + draw%8);
            validate(&declaration, buffers, offsets);
        }
        if (!frame) { assert(pointers + ipointers - old_calls == 2400); old_calls = pointers + ipointers; }
        else assert(pointers + ipointers == old_calls);
    }
    assert(vertex_array_cache.count == 400);
    old_binds = binds; old_elements = element_binds;
    assert(vertex_array_cache_use(&declaration, 3, buffers, offsets));
    state_element_array_buffer(207);
    assert(binds == old_binds && element_binds == old_elements); /* Current VAO hits do not rebind. */
    invalidate();
    assert(vertex_array_cache_use(&declaration, 3, buffers, offsets));
    state_element_array_buffer(207);
    assert(element_binds == old_elements + 1 && pointers + ipointers == old_calls); /* Epoch invalidates index knowledge. */
    offsets[0]++;
    assert(vertex_array_cache_use(&declaration, 3, buffers, offsets)); validate(&declaration, buffers, offsets);
    assert(pointers + ipointers == old_calls + 6);
    device.streams[1].stride = 20;
    assert(vertex_array_cache_use(&declaration, 3, buffers, offsets)); validate(&declaration, buffers, offsets);
    assert(pointers + ipointers == old_calls + 12);
    buffers[0]++;
    assert(vertex_array_cache_use(&declaration, 3, buffers, offsets)); validate(&declaration, buffers, offsets);
    assert(pointers + ipointers == old_calls + 18);
    other = declaration;
    assert(vertex_array_cache_use(&other, 3, buffers, offsets)); validate(&other, buffers, offsets);
    assert(pointers + ipointers == old_calls + 24);
    vertex_array_cache_bind(NULL); state_element_array_buffer(999);
    assert(vertex_array_cache_use(&declaration, 3, buffers, offsets));
    vertex_array_cache_bind(NULL);
    assert(current_vao == device.vertex_array && actual[current_vao].element == 999 && gl_state.element_array_buffer == 999);
    allocation_failure = 1; offsets[0]++;
    assert(!vertex_array_cache_use(&declaration, 3, buffers, offsets));
    vertex_array_cache_bind(NULL); baseline(&declaration, buffers, offsets); validate(&declaration, buffers, offsets);
    allocation_failure = 0;
    vao_failure = 1;
    assert(!vertex_array_cache_use(&declaration, 3, buffers, offsets));
    assert(vertex_array_cache.count == 404);
    vao_failure = 0;
    for (unsigned draw=0; vertex_array_cache.count < VERTEX_ARRAY_CACHE_MAXIMUM; draw++) {
        offsets[0] = 1000000 + draw*64; offsets[1] = draw*32;
        assert(vertex_array_cache_use(&declaration, 3, buffers, offsets)); validate(&declaration, buffers, offsets);
    }
    offsets[0] = offsets[1] = 5000000;
#ifdef HALO_ANDROID
    old_calls = next_vao;
    assert(!vertex_array_cache_use(&declaration, 3, buffers, offsets));
    vertex_array_cache_bind(NULL); baseline(&declaration, buffers, offsets); validate(&declaration, buffers, offsets);
    assert(vertex_array_cache.count == VERTEX_ARRAY_CACHE_MAXIMUM && next_vao == old_calls && !deletions);
    device.streams[1].stride = 16; buffers[0] = 101;
    offsets[0] = offsets[1] = 0;
    assert(vertex_array_cache_use(&declaration, 3, buffers, offsets) && current_vao == saved[0]);
#else
    assert(vertex_array_cache_use(&declaration, 3, buffers, offsets));
    assert(vertex_array_cache.count == 1 && deletions == VERTEX_ARRAY_CACHE_MAXIMUM);
    vertex_array_cache_clear(); assert(vertex_array_cache.count == 0 && current_vao == device.vertex_array);
#endif
    puts("PASS: actual VAO cache; 400 draws x 6 attributes: 2400 pointer calls cold, 0 warm vs 2400 baseline/warm");
    puts("PASS: buffer/offset/stride/declaration, integer attributes, indices, epoch, immediate fallback, OOM, VAO failure, bounded capacity");
    return 0;
}
'''

def target(path):
    return "/mnt/" + path.drive[0].lower() + path.as_posix()[2:] if args.wsl else str(path)

prefix = ["wsl", "-d", args.wsl, "--"] if args.wsl else []
(root / "build").mkdir(exist_ok=True)
with tempfile.TemporaryDirectory(prefix="vao-cache-test-", dir=root / "build") as temporary:
    c_file = Path(temporary) / "test.c"
    binary = Path(temporary) / "test"
    c_file.write_text(program, encoding="utf-8", newline="\n")
    subprocess.run([*prefix, "gcc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", target(c_file), "-o", target(binary)], check=True)
    subprocess.run([*prefix, target(binary)], check=True)
