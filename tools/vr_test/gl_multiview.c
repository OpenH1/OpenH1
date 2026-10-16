/*
An asset-free GPU check of the GLSL emitter's eye transforms. The authored
programs move a triangle directly to clip space, or project it and perform
the Xbox viewport conversion. Every pixel of each view must match an
independent matrix-only GLSL program, for two and four views.

The projection case catches NVIDIA's OVR multiview compiler reusing a
captured position after an in-place left-eye transform: with translations
-0.25 and +0.25 the right centroid was 128 instead of 160.
*/

#define GL_GLEXT_PROTOTYPES
#include <GL/glcorearb.h>
#include <SDL3/SDL.h>
#include "xgpu_gl.h"
#include "xgpu_shader.h"
#include <stdio.h>
#include <math.h>
#include <assert.h>
void platform_log(const char *format, ...)
{
	(void)format;
}
static GLuint compile(GLenum type, const char *text)
{
	GLuint shader = glCreateShader(type);
	GLint ok;
	char log[8192];
	glShaderSource(shader, 1, &text, NULL);
	glCompileShader(shader);
	glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
	if (!ok)
	{
		glGetShaderInfoLog(shader, sizeof(log), NULL, log);
		puts(log);
		puts(text);
		abort();
	}
	return shader;
}
static unsigned char pixels[2][4][256 * 256 * 4];
int main(void)
{
	SDL_Window *window;
	SDL_GLContext context;
	PFNGLFRAMEBUFFERTEXTUREMULTIVIEWOVRPROC multiview;
	GLuint shaders[2], programs[2], fragment, texture, fb, readfb, vao, vbo;
	int i, which, eye, cases, projected, foveated, views;
	GLint okay;
	struct xgpu_vertex_program program = {0};
	struct xgpu_glsl_options options = {.version = "450 core"};
	float constants[XGPU_VERTEX_CONSTANT_COUNT][4] = {{0}}, eyes[2][16], screen[2][4] = {{1, 1, 0, 0}, {1, 1, 0, 0}};
	float fovea[4][4] = {{1, 1, 0, 0}, {1, 1, 0, 0}, {1, 1, 0, 0}, {1, 1, 0, 0}};
	float vertices[3][4] = {{-.2, -.2, .7, 1}, {.2, -.2, .7, 1}, {0, .2, .7, 1}};
	char *source;
	char reference[1024];
	const char *solid = "#version 450\nout vec4 color;void main(){color=vec4(1);}";
	assert(SDL_Init(SDL_INIT_VIDEO));
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 5);
	window = SDL_CreateWindow("GL multiview check", 256, 256, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
	assert(window);
	context = SDL_GL_CreateContext(window);
	assert(context);
	printf("GL: %s %s\n", glGetString(GL_VENDOR), glGetString(GL_RENDERER));
	multiview =
		(PFNGLFRAMEBUFFERTEXTUREMULTIVIEWOVRPROC)SDL_GL_GetProcAddress("glFramebufferTextureMultiviewOVR");
	assert(multiview);
	if (!SDL_GL_ExtensionSupported("GL_OVR_multiview2"))
	{
		puts("GL_OVR_multiview2 unavailable");
		SDL_GL_DestroyContext(context);
		SDL_DestroyWindow(window);
		SDL_Quit();
		return 77;
	}
	for (projected = 0; projected < 2; projected++)
		for (foveated = 0; foveated < 2; foveated++)
		{
			views = foveated ? 4 : 2;
			options.foveated = foveated;
			memset(&program, 0, sizeof(program));
			snprintf(
				reference, sizeof(reference),
				"#version 450 core\n#extension GL_OVR_multiview2 : require\n"
				"layout(num_views=%d) in;\nlayout(location=0) in vec4 v0;uniform mat4 eye_correction[2];"
				"void main(){gl_Position=eye_correction[gl_ViewID_OVR/%d]*v0;gl_Position.y=-gl_Position.y;}",
				views, foveated ? 2 : 1);
			program.instruction_count = 2;
			program.projects = 1;
			program.instructions[0].mac = XGPU_MAC_MOV;
			program.instructions[0].a.file = XGPU_VERTEX_INPUT;
			for (i = 0; i < 4; i++)
				program.instructions[0].a.swizzle[i] = i;
			program.instructions[0].writes_output = 1;
			program.instructions[0].output = XGPU_OUTPUT_POSITION;
			program.instructions[0].output_mask = 15;
			program.instructions[1].captures_clip = 1;

			if (projected)
			{
				memset(&program, 0, sizeof(program));
				program.instruction_count = 6;
				program.projects = 1;
				for (i = 0; i < 4; i++)
				{
					struct xgpu_vertex_instruction *ins = &program.instructions[i];
					int j;
					ins->mac = XGPU_MAC_DPH;
					ins->a.file = XGPU_VERTEX_INPUT;
					ins->b.file = XGPU_VERTEX_CONSTANT;
					ins->b.index = i;
					for (j = 0; j < 4; j++)
					{
						ins->a.swizzle[j] = j;
						ins->b.swizzle[j] = j;
					}
					ins->writes_output = 1;
					ins->output = XGPU_OUTPUT_POSITION;
					ins->output_mask = 8 >> i;
				}
				/* Keep clip space before scale * rcc(w) + offset reuses oPos. */
				{
					struct xgpu_vertex_instruction *ins = &program.instructions[4];
					int j;
					ins->mac = XGPU_MAC_MUL;
					ins->a.file = XGPU_VERTEX_TEMPORARY;
					ins->a.index = XGPU_VERTEX_POSITION;
					ins->b.file = XGPU_VERTEX_CONSTANT;
					ins->b.index = XGPU_VERTEX_CONSTANT_BIAS - 38;
					ins->ilu = XGPU_ILU_RCC;
					ins->c.file = XGPU_VERTEX_TEMPORARY;
					ins->c.index = XGPU_VERTEX_POSITION;
					for (j = 0; j < 4; j++)
					{
						ins->a.swizzle[j] = j;
						ins->b.swizzle[j] = j;
						ins->c.swizzle[j] = 3;
					}
					ins->writes_output = 1;
					ins->output = XGPU_OUTPUT_POSITION;
					ins->output_mask = 14;
					ins->ilu_temporary = 0;
					ins->ilu_mask = 8;
					ins->captures_clip = 1;
				}
				{
					struct xgpu_vertex_instruction *ins = &program.instructions[5];
					int j;
					ins->mac = XGPU_MAC_MAD;
					ins->a.file = XGPU_VERTEX_TEMPORARY;
					ins->a.index = XGPU_VERTEX_POSITION;
					ins->b.file = XGPU_VERTEX_TEMPORARY;
					ins->b.index = 0;
					ins->c.file = XGPU_VERTEX_CONSTANT;
					ins->c.index = XGPU_VERTEX_CONSTANT_BIAS - 37;
					for (j = 0; j < 4; j++)
					{
						ins->a.swizzle[j] = j;
						ins->b.swizzle[j] = 0;
						ins->c.swizzle[j] = j;
					}
					ins->writes_output = 1;
					ins->output = XGPU_OUTPUT_POSITION;
					ins->output_mask = 14;
				}
			}
			source = xgpu_glsl_vertex_shader(&program, 0, NULL, XGPU_MULTIVIEW_EYES, &options);

			shaders[0] = compile(GL_VERTEX_SHADER, source);
			free(source);
			shaders[1] = compile(GL_VERTEX_SHADER, reference);
			fragment = compile(GL_FRAGMENT_SHADER, solid);
			for (i = 0; i < 2; i++)
			{
				programs[i] = glCreateProgram();
				glAttachShader(programs[i], shaders[i]);
				glAttachShader(programs[i], fragment);
				glLinkProgram(programs[i]);
				glGetProgramiv(programs[i], GL_LINK_STATUS, &okay);
				assert(okay);
			}
			glGenTextures(1, &texture);
			glBindTexture(GL_TEXTURE_2D_ARRAY, texture);
			glTexStorage3D(GL_TEXTURE_2D_ARRAY, 1, GL_RGBA8, 256, 256, views);
			glGenFramebuffers(1, &fb);
			glGenFramebuffers(1, &readfb);
			glBindFramebuffer(GL_FRAMEBUFFER, fb);
			multiview(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, texture, 0, 0, views);
			assert(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
			glGenVertexArrays(1, &vao);
			glBindVertexArray(vao);
			glGenBuffers(1, &vbo);
			glBindBuffer(GL_ARRAY_BUFFER, vbo);
			glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
			glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 16, NULL);
			glEnableVertexAttribArray(0);
			glClipControl(GL_UPPER_LEFT, GL_ZERO_TO_ONE);
			glViewport(0, 0, 256, 256);
			glDisable(GL_DEPTH_TEST);
			glDisable(GL_CULL_FACE);
			for (i = 0; i < 4; i++)
				constants[i][i] = 1;
			constants[XGPU_VERTEX_CONSTANT_BIAS - 38][0] = constants[XGPU_VERTEX_CONSTANT_BIAS - 38][1] = constants[XGPU_VERTEX_CONSTANT_BIAS - 38][2] = 1;
			constants[XGPU_VERTEX_CONSTANT_BIAS - 37][0] = constants[XGPU_VERTEX_CONSTANT_BIAS - 37][1] = -.5f;
			for (cases = 0; cases < 2; cases++)
			{
				double means[2][4] = {{0}};
				memset(eyes, 0, sizeof(eyes));
				for (eye = 0; eye < 2; eye++)
				{
					for (i = 0; i < 4; i++)
						eyes[eye][i * 5] = 1;
					if (!cases)
						eyes[eye][3] = eye ? .25f : -.25f;
					else
					{
						eyes[eye][0] = eyes[eye][5] = .75f;
						eyes[eye][2] = eye ? .125f : -.125f;
						eyes[eye][3] = eye ? -.125f : .125f;
					}
				}
				for (which = 0; which < 2; which++)
				{
					GLuint p = programs[which];
					glUseProgram(p);
					glUniformMatrix4fv(glGetUniformLocation(p, "eye_correction"), 2, GL_TRUE, &eyes[0][0]);
					{
						float actual[16];
						glGetUniformfv(p, glGetUniformLocation(p, "eye_correction[1]"), actual);
						assert(actual[12] == eyes[1][3]);
					}
					if (!which)
					{
						glUniform4fv(glGetUniformLocation(p, "c"), XGPU_VERTEX_CONSTANT_COUNT, &constants[0][0]);
						glUniform4f(glGetUniformLocation(p, "viewport_scale"), 1, 1, 1, 0);
						glUniform4f(glGetUniformLocation(p, "viewport_offset"), 0, 0, 0, 0);
						glUniform1f(glGetUniformLocation(p, "point_size"), 1);
						glUniform4fv(glGetUniformLocation(p, "screen_correction"), 2, &screen[0][0]);
						glUniform1f(glGetUniformLocation(p, "screen_points"), 0);
						if (foveated)
							glUniform4fv(glGetUniformLocation(p, "fovea_view"), 4, &fovea[0][0]);
					}
					glBindFramebuffer(GL_FRAMEBUFFER, fb);
					glClearColor(0, 0, 0, 0);
					glClear(GL_COLOR_BUFFER_BIT);
					glDrawArrays(GL_TRIANGLES, 0, 3);
					glBindFramebuffer(GL_FRAMEBUFFER, readfb);
					for (eye = 0; eye < views; eye++)
					{
						unsigned count = 0;
						double sum = 0;
						glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, texture, 0, eye);
						glReadPixels(0, 0, 256, 256, GL_RGBA, GL_UNSIGNED_BYTE, pixels[which][eye]);
						for (i = 0; i < 256 * 256; i++)
							if (pixels[which][eye][i * 4])
							{
								sum += (i % 256) + .5;
								count++;
							}
						assert(count > 100);
						means[which][eye] = sum / count;
						printf("projected%d views%d case%d %s view%d pixels%u meanX%.6f\n", projected, views,
							   cases, which ? "reference" : "GL emitter", eye, count, means[which][eye]);
					}
					assert(glGetError() == GL_NO_ERROR);
				}
				for (eye = 0; eye < views; eye++)
					assert(!memcmp(pixels[0][eye], pixels[1][eye], sizeof(pixels[0][eye])));
				if (!cases)
					for (eye = 0; eye < views; eye++)
						assert(fabs(means[0][eye] - (eye / (foveated ? 2 : 1) ? 160 : 96)) < .1);
			}
			for (which = 0; which < 2; which++)
			{
				glDeleteProgram(programs[which]);
				glDeleteShader(shaders[which]);
			}
			glDeleteShader(fragment);
			glDeleteTextures(1, &texture);
			glDeleteFramebuffers(1, &fb);
			glDeleteFramebuffers(1, &readfb);
			glDeleteVertexArrays(1, &vao);
			glDeleteBuffers(1, &vbo);
		}
	puts("GL multiview emitter/reference pixels identical");
	SDL_GL_DestroyContext(context);
	SDL_DestroyWindow(window);
	SDL_Quit();
	return 0;
}
