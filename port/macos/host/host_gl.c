/*
HOST_GL.C

OpenGL ES for the guest, beyond the functions the generated bridge
(host_gl_bridge.c, tools/wasm_gl_stubs.py) passes straight to the driver:
strings, which need copying into guest memory, and the renderer's helpers
that work on the driver's own objects (mappings and fences), which the guest
cannot hold. The driver is ANGLE; its functions come from the table in
host_gl_functions.h, which host_gl_load fills once a context exists.
*/

#include "host.h"
#include "host_gl_functions.h"

#include <string.h>

/* copies glGetString(name), or glGetStringi(name, index) when index >= 0 */
void w2c_halo_host_gl_get_string(struct w2c_halo *instance, u32 name, u32 index, u32 buffer, u32 size)
{
	char *destination = host_guest_pointer(buffer);
	const GLubyte *text = NULL;

	(void)instance;
	if (!destination || !size)
		return;
	destination[0] = 0;
	if ((s32)index >= 0)
	{
		if (host_glGetStringi)
			text = host_glGetStringi((GLenum)name, index);
	}
	else if (host_glGetString)
	{
		text = host_glGetString((GLenum)name);
	}
	if (text)
	{
		strncpy(destination, (const char *)text, size - 1);
		destination[size - 1] = 0;
	}
}

u32 w2c_halo_host_gl_has_extension(struct w2c_halo *instance, u32 name)
{
	const char *wanted = host_guest_pointer(name);
	GLint count = 0, index;

	(void)instance;
	if (!wanted || !host_glGetIntegerv || !host_glGetStringi)
		return 0;
	host_glGetIntegerv(GL_NUM_EXTENSIONS, &count);
	for (index = 0; index < count; index++)
	{
		const char *extension = (const char *)host_glGetStringi(GL_EXTENSIONS, (GLuint)index);

		if (extension && !strcmp(extension, wanted))
			return 1;
	}
	return 0;
}

/* one 32-bit word of a buffer object (the visibility test counters of
d3d8_gl.c); ES has no glGetBufferSubData, and the mapping it offers
instead is a host pointer. The counters are atomic counter buffers, which
only ES 3.1 has: without them (ANGLE's ES 3.0) the binding errors and
nothing is mapped, so this reads 0 rather than crashing */
u32 w2c_halo_host_gl_read_buffer_word(struct w2c_halo *instance, u32 buffer, u32 offset)
{
	static int checked, supported;
	uint32_t value = 0;
	GLint previous = 0;
	const void *mapping;

	(void)instance;
	if (!checked)
	{
		GLint major = 0, minor = 0;

		host_glGetIntegerv(GL_MAJOR_VERSION, &major);
		host_glGetIntegerv(GL_MINOR_VERSION, &minor);
		supported = major > 3 || (major == 3 && minor >= 1);
		checked = 1;
		if (!supported)
			host_logf("OpenGL ES %d.%d has no atomic counter buffers to read", (int)major, (int)minor);
	}
	if (!supported)
		return 0;
	host_glGetIntegerv(GL_ATOMIC_COUNTER_BUFFER_BINDING, &previous);
	host_glBindBuffer(GL_ATOMIC_COUNTER_BUFFER, buffer);
	mapping = host_glMapBufferRange(GL_ATOMIC_COUNTER_BUFFER, offset, sizeof(value), GL_MAP_READ_BIT);
	if (mapping)
	{
		memcpy(&value, mapping, sizeof(value));
		host_glUnmapBuffer(GL_ATOMIC_COUNTER_BUFFER);
	}
	host_glBindBuffer(GL_ATOMIC_COUNTER_BUFFER, (GLuint)previous);
	return value;
}

/* The renderer streams each frame's vertices and indices into the next of
a ring of buffers (d3d8_gl.c). A fence marks the end of each frame's work,
and a buffer is written again only once the GPU has passed the fence of the
frame that last used it: drivers queue several frames, and a draw still
waiting to run would otherwise read a later frame's vertices. */
#define FRAME_FENCE_SLOTS 8

static GLsync frame_fences[FRAME_FENCE_SLOTS];

void w2c_halo_host_gl_fence_frame(struct w2c_halo *instance, u32 slot)
{
	(void)instance;
	if (slot >= FRAME_FENCE_SLOTS)
		return;
	if (frame_fences[slot])
		host_glDeleteSync(frame_fences[slot]);
	frame_fences[slot] = host_glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
}

void w2c_halo_host_gl_wait_frame(struct w2c_halo *instance, u32 slot)
{
	(void)instance;
	if (slot >= FRAME_FENCE_SLOTS || !frame_fences[slot])
		return;
	/* at most a second: a lost context must not hang the game */
	host_glClientWaitSync(frame_fences[slot], GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ull);
	host_glDeleteSync(frame_fences[slot]);
	frame_fences[slot] = NULL;
}

/* writes data into the buffer bound to target. The renderer streams a
range per draw, so a frame makes hundreds of these, and the cost per call
rather than per byte is what a frame is made of. Unsynchronized, without
invalidating the range (which on some drivers costs as much as a stall;
see port/android/host/host_gl.c): the renderer only writes ranges that no
queued draw reads, because host_gl_wait_frame releases the ring slot
first. */
void w2c_halo_host_gl_buffer_write(struct w2c_halo *instance, u32 target, u32 offset, u32 size, u32 data)
{
	const void *source = host_guest_pointer(data);
	void *mapping;

	(void)instance;
	if (!size || !source)
		return;
	mapping = host_glMapBufferRange((GLenum)target, (GLintptr)offset, (GLsizeiptr)size,
		GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT);
	if (!mapping)
	{
		host_glBufferSubData((GLenum)target, (GLintptr)offset, (GLsizeiptr)size, source);
		return;
	}
	memcpy(mapping, source, size);
	host_glUnmapBuffer((GLenum)target);
}
