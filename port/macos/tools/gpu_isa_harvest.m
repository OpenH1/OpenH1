/*
GPU_ISA_HARVEST.M

The GPU code Metal compiles a function to on this Mac, harvested: a .metal
source or a .metallib is opened, one function made (specialised with its
function constants' values if given), a pipeline of it added to a binary
archive, and the archive written; the archive holds the GPU's machine code,
which tools/gpu_isa.py extracts and disassembles.

	gpu_isa_harvest --output <archive> --entry <function> [--vertex <function>
		[--vertex-source <file.metal>]] [--constant <index>=<value>]... [--math fast|relaxed|safe]
		<file.metal | file.metallib>

A source is compiled with fast maths unless --math says otherwise (as
MTLCompileOptions' mathMode). A constant's value is read as its declared type (bool, int, uint, half,
float, short, ushort, char, uchar). A fragment function is added to a
render pipeline with the vertex function --vertex names (from
--vertex-source, or the same file); its colour target is RGBA8Unorm.
*/

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void fail(NSString *what, NSError *error)
{
	fprintf(stderr, "gpu_isa_harvest: %s%s%s\n", what.UTF8String, error ? ": " : "",
		error ? error.localizedDescription.UTF8String : "");
	exit(1);
}

/* a constant's value as its declared type, into values */
static void set_constant(MTLFunctionConstantValues *values, MTLFunctionConstant *constant, const char *text)
{
	union { bool b; int32_t i; uint32_t u; int16_t s; uint16_t us; int8_t c; uint8_t uc; float f; __fp16 h; } value;

	memset(&value, 0, sizeof(value));
	switch (constant.type)
	{
	case MTLDataTypeBool: value.b = strtol(text, NULL, 0) != 0; break;
	case MTLDataTypeInt: value.i = (int32_t)strtol(text, NULL, 0); break;
	case MTLDataTypeUInt: value.u = (uint32_t)strtoul(text, NULL, 0); break;
	case MTLDataTypeShort: value.s = (int16_t)strtol(text, NULL, 0); break;
	case MTLDataTypeUShort: value.us = (uint16_t)strtoul(text, NULL, 0); break;
	case MTLDataTypeChar: value.c = (int8_t)strtol(text, NULL, 0); break;
	case MTLDataTypeUChar: value.uc = (uint8_t)strtoul(text, NULL, 0); break;
	case MTLDataTypeFloat: value.f = strtof(text, NULL); break;
	case MTLDataTypeHalf: value.h = (__fp16)strtof(text, NULL); break;
	default: fail([NSString stringWithFormat:@"constant %@: a type this tool does not set", constant.name], nil);
	}
	[values setConstantValue:&value type:constant.type atIndex:constant.index];
}

int main(int argc, char **argv)
{
	@autoreleasepool
	{
		static const struct option options[] = {
			{ "output", required_argument, NULL, 'o' },
			{ "entry", required_argument, NULL, 'e' },
			{ "vertex", required_argument, NULL, 'v' },
			{ "vertex-source", required_argument, NULL, 'V' },
			{ "constant", required_argument, NULL, 'c' },
			{ "math", required_argument, NULL, 'm' },
			{ NULL, 0, NULL, 0 },
		};
		const char *output = NULL, *entry = NULL, *vertex_name = NULL, *vertex_source = NULL;
		NSMutableDictionary<NSNumber *, NSString *> *given = [NSMutableDictionary dictionary];
		MTLCompileOptions *compile = [MTLCompileOptions new];
		id<MTLDevice> device = MTLCreateSystemDefaultDevice();
		id<MTLLibrary> library;
		id<MTLFunction> function, vertex = nil;
		id<MTLBinaryArchive> archive;
		NSError *error = nil;
		NSString *path;
		int c;

		while ((c = getopt_long(argc, argv, "o:e:v:V:c:m:", options, NULL)) != -1)
		{
			switch (c)
			{
			case 'o': output = optarg; break;
			case 'e': entry = optarg; break;
			case 'v': vertex_name = optarg; break;
			case 'V': vertex_source = optarg; break;
			case 'c':
			{
				const char *equals = strchr(optarg, '=');

				if (!equals)
					fail(@"--constant takes <index>=<value>", nil);
				given[@(atoi(optarg))] = @(equals + 1);
				break;
			}
			case 'm':
				if (!strcmp(optarg, "safe"))
					compile.mathMode = MTLMathModeSafe;
				else if (!strcmp(optarg, "relaxed"))
					compile.mathMode = MTLMathModeRelaxed;
				else if (!strcmp(optarg, "fast"))
					compile.mathMode = MTLMathModeFast;
				else
					fail(@"--math takes fast, relaxed or safe", nil);
				break;
			default: fail(@"usage: --output <archive> --entry <function> [--vertex <function> [--vertex-source <file>]] "
				"[--constant <index>=<value>]... [--math fast|relaxed|safe] <file>", nil);
			}
		}
		if (!output || !entry || optind != argc - 1)
			fail(@"usage: --output <archive> --entry <function> [--vertex <function> [--vertex-source <file>]] "
				"[--constant <index>=<value>]... [--math fast|relaxed|safe] <file>", nil);
		if (!device)
			fail(@"no Metal device", nil);
		path = @(argv[optind]);
		if ([path hasSuffix:@".metallib"])
			library = [device newLibraryWithURL:[NSURL fileURLWithPath:path] error:&error];
		else
		{
			NSString *source = [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:&error];

			if (!source)
				fail([NSString stringWithFormat:@"cannot read %@", path], error);
			library = [device newLibraryWithSource:source options:compile error:&error];
		}
		if (!library)
			fail(@"cannot compile the library", error);
		function = [library newFunctionWithName:@(entry)];
		if (!function)
			fail([NSString stringWithFormat:@"no function %s", entry], nil);
		if (function.functionConstantsDictionary.count)
		{
			MTLFunctionConstantValues *values = [MTLFunctionConstantValues new];

			for (MTLFunctionConstant *constant in function.functionConstantsDictionary.allValues)
			{
				NSString *text = given[@(constant.index)];

				if (text)
					set_constant(values, constant, text.UTF8String);
				else if (constant.required)
					fail([NSString stringWithFormat:@"constant %lu (%@) needs a value", (unsigned long)constant.index,
						constant.name], nil);
			}
			function = [library newFunctionWithName:@(entry) constantValues:values error:&error];
			if (!function)
				fail(@"cannot specialise", error);
		}
		if (vertex_name)
		{
			id<MTLLibrary> vertex_library = library;

			if (vertex_source)
			{
				NSString *source = [NSString stringWithContentsOfFile:@(vertex_source) encoding:NSUTF8StringEncoding
					error:&error];

				if (!source)
					fail([NSString stringWithFormat:@"cannot read %s", vertex_source], error);
				vertex_library = [device newLibraryWithSource:source options:compile error:&error];
				if (!vertex_library)
					fail(@"cannot compile the vertex function's library", error);
			}
			vertex = [vertex_library newFunctionWithName:@(vertex_name)];
			if (!vertex)
				fail([NSString stringWithFormat:@"no function %s", vertex_name], nil);
		}
		archive = [device newBinaryArchiveWithDescriptor:[MTLBinaryArchiveDescriptor new] error:&error];
		if (!archive)
			fail(@"cannot make a binary archive", error);
		if (function.functionType == MTLFunctionTypeKernel)
		{
			MTLComputePipelineDescriptor *descriptor = [MTLComputePipelineDescriptor new];

			descriptor.computeFunction = function;
			if (![archive addComputePipelineFunctionsWithDescriptor:descriptor error:&error])
				fail(@"cannot add the compute pipeline", error);
		}
		else
		{
			MTLRenderPipelineDescriptor *descriptor = [MTLRenderPipelineDescriptor new];

			if (function.functionType == MTLFunctionTypeVertex)
				descriptor.vertexFunction = function;
			else
			{
				descriptor.vertexFunction = vertex;
				descriptor.fragmentFunction = function;
			}
			descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
			if (![archive addRenderPipelineFunctionsWithDescriptor:descriptor error:&error])
				fail(@"cannot add the render pipeline", error);
		}
		if (![archive serializeToURL:[NSURL fileURLWithPath:@(output)] error:&error])
			fail([NSString stringWithFormat:@"cannot write %s", output], error);
		printf("%s\n", device.name.UTF8String);
	}
	return 0;
}
