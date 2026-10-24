/*
SYNTHETIC.C

Shader programs made up to use every part of the intermediate form the
captured ones may not (msl_corpus.c): each texture stage mode and dot
mapping, each combiner input register and mapping, output mapping and
destination, the dot products, muxes and blue-to-alpha copies, the final
combiner's inputs, sum and product, each fog mode, alpha test function,
signed channel and the draw's features; and each vertex program operation.
synthetic_pixel_programs and synthetic_vertex_programs call back with each
and its name.
*/

typedef void (*synthetic_pixel_callback)(const char *name, const struct xgpu_pixel_program *program);
typedef void (*synthetic_vertex_callback)(const char *name, const struct xgpu_vertex_program *program);

/* r0 = t0 + t1, r1 = t2 + t3 (each portion), the final combiner r0 + r1 */
static void synthetic_base(struct xgpu_pixel_program *program)
{
	struct xgpu_combiner_portion *part;
	int portion;

	memset(program, 0, sizeof(*program));
	program->combiner_count = 1;
	for (portion = 0; portion < 2; portion++)
	{
		part = &program->combiners[0].portions[portion];
		part->inputs[0].reg = XGPU_REGISTER_T0;
		part->inputs[1].reg = XGPU_REGISTER_T1;
		part->inputs[2].reg = XGPU_REGISTER_T2;
		part->inputs[3].reg = XGPU_REGISTER_T3;
		part->inputs[0].alpha = part->inputs[1].alpha = part->inputs[2].alpha = part->inputs[3].alpha = portion;
		part->inputs[0].mapping = part->inputs[2].mapping = XGPU_MAP_SIGNED_IDENTITY;
		part->inputs[1].mapping = part->inputs[3].mapping = XGPU_MAP_UNSIGNED_INVERT;
		part->ab = XGPU_REGISTER_R0;
		part->cd = XGPU_REGISTER_R1;
	}
	program->final.present = 1;
	program->final.inputs[0].reg = XGPU_REGISTER_ZERO;
	program->final.inputs[3].reg = XGPU_REGISTER_R0;
	program->final.inputs[6].reg = XGPU_REGISTER_R1;
	program->final.inputs[6].alpha = 1;
}

static void synthetic_stage(struct xgpu_pixel_program *program, int stage, unsigned char mode, unsigned char sampler)
{
	struct xgpu_texture_stage *decoded = &program->stages[stage];

	decoded->declared_mode = decoded->mode = mode;
	decoded->sampler = sampler;
	decoded->previous = (signed char)(stage - 1);
	decoded->input = stage ? (unsigned char)(stage - 1) : 0;
}

static void synthetic_pixel_programs(synthetic_pixel_callback callback)
{
	struct xgpu_pixel_program program;
	char name[64];
	int i, j, stage;

	/* the texture stage modes, as each is used */
	for (stage = 0; stage < 4; stage++)
	{
		static const unsigned char modes[] = { XGPU_TEXTURE_PROJECT2D, XGPU_TEXTURE_PROJECT3D, XGPU_TEXTURE_CUBEMAP,
			XGPU_TEXTURE_PASSTHRU, XGPU_TEXTURE_CLIPPLANE };
		static const unsigned char samplers[] = { _xgpu_sampler_2d, _xgpu_sampler_3d, _xgpu_sampler_cube, _xgpu_sampler_none,
			_xgpu_sampler_none };

		for (i = 0; i < (int)sizeof(modes); i++)
		{
			synthetic_base(&program);
			synthetic_stage(&program, stage, modes[i], samplers[i]);
			program.stages[stage].compare = (unsigned char)(stage * 5 + i);
			program.stages[stage].alpha_kill = i == 1;
			program.stages[stage].color_sign = (unsigned char)((stage * 3 + i) & 15);
			snprintf(name, sizeof(name), "synthetic-stage%d-mode%d", stage, modes[i]);
			callback(name, &program);
		}
	}
	for (i = 0; i < 2; i++)
	{
		synthetic_base(&program);
		synthetic_stage(&program, 0, XGPU_TEXTURE_PROJECT2D, _xgpu_sampler_2d);
		synthetic_stage(&program, 1, i ? XGPU_TEXTURE_BUMPENVMAP_LUMINANCE : XGPU_TEXTURE_BUMPENVMAP, _xgpu_sampler_2d);
		synthetic_stage(&program, 2, XGPU_TEXTURE_BUMPENVMAP, _xgpu_sampler_3d);
		snprintf(name, sizeof(name), "synthetic-bump%d", i);
		callback(name, &program);
	}
	for (i = 0; i < 2; i++)
	{
		synthetic_base(&program);
		synthetic_stage(&program, 0, XGPU_TEXTURE_PROJECT2D, _xgpu_sampler_2d);
		synthetic_stage(&program, 1, i ? XGPU_TEXTURE_DEPENDENT_GB : XGPU_TEXTURE_DEPENDENT_AR, _xgpu_sampler_2d);
		synthetic_stage(&program, 2, i ? XGPU_TEXTURE_DEPENDENT_AR : XGPU_TEXTURE_DEPENDENT_GB, _xgpu_sampler_2d);
		program.stages[2].input = 0;
		snprintf(name, sizeof(name), "synthetic-dependent%d", i);
		callback(name, &program);
	}
	/* a z-sprite (the game's effect program with its second map's puff:
	stage 3's DOT_ZW replaces the fragment's depth, xgpu_shader.h) */
	synthetic_base(&program);
	synthetic_stage(&program, 0, XGPU_TEXTURE_PROJECT2D, _xgpu_sampler_2d);
	synthetic_stage(&program, 1, XGPU_TEXTURE_PROJECT2D, _xgpu_sampler_2d);
	synthetic_stage(&program, 2, XGPU_TEXTURE_DOT_PRODUCT, _xgpu_sampler_none);
	synthetic_stage(&program, 3, XGPU_TEXTURE_DOT_ZW, _xgpu_sampler_none);
	program.stages[2].input = program.stages[3].input = 1;
	callback("synthetic-zsprite", &program);
	/* the dot products, with each dot mapping */
	for (i = 0; i < 8; i++)
	{
		static const unsigned char last[][3] =
		{
			{ XGPU_TEXTURE_DOT_PRODUCT, XGPU_TEXTURE_DOT_ST, _xgpu_sampler_2d },
			{ XGPU_TEXTURE_DOT_PRODUCT, XGPU_TEXTURE_DOT_ZW, _xgpu_sampler_none },
			{ XGPU_TEXTURE_DOT_PRODUCT, XGPU_TEXTURE_DOT_PRODUCT, _xgpu_sampler_none },
		};

		for (j = 0; j < 3; j++)
		{
			synthetic_base(&program);
			synthetic_stage(&program, 0, XGPU_TEXTURE_PROJECT2D, _xgpu_sampler_2d);
			synthetic_stage(&program, 1, last[j][0], _xgpu_sampler_none);
			synthetic_stage(&program, 2, last[j][1], last[j][2]);
			program.stages[1].input = program.stages[2].input = 0;
			program.stages[1].dot_mapping = program.stages[2].dot_mapping = (unsigned char)i;
			snprintf(name, sizeof(name), "synthetic-dot%d-mapping%d", j, i);
			callback(name, &program);
		}
		for (j = 0; j < 4; j++)
		{
			static const unsigned char modes[] = { XGPU_TEXTURE_DOT_REFLECT_SPECULAR, XGPU_TEXTURE_DOT_REFLECT_SPECULAR_CONSTANT,
				XGPU_TEXTURE_DOT_STR_3D, XGPU_TEXTURE_DOT_STR_CUBE };

			synthetic_base(&program);
			synthetic_stage(&program, 0, XGPU_TEXTURE_PROJECT2D, _xgpu_sampler_2d);
			synthetic_stage(&program, 1, XGPU_TEXTURE_DOT_PRODUCT, _xgpu_sampler_none);
			synthetic_stage(&program, 2, j == 0 && (i & 1) ? XGPU_TEXTURE_DOT_REFLECT_DIFFUSE : XGPU_TEXTURE_DOT_PRODUCT,
				j == 0 && (i & 1) ? _xgpu_sampler_cube : _xgpu_sampler_none);
			synthetic_stage(&program, 3, modes[j], j == 2 ? _xgpu_sampler_3d : _xgpu_sampler_cube);
			program.stages[1].input = program.stages[2].input = program.stages[3].input = 0;
			program.stages[1].dot_mapping = program.stages[2].dot_mapping = program.stages[3].dot_mapping = (unsigned char)i;
			snprintf(name, sizeof(name), "synthetic-reflect%d-mapping%d", j, i);
			callback(name, &program);
		}
	}
	/* each register through each input mapping, in both portions */
	for (i = 0; i < 16; i++)
	{
		for (j = 0; j < 8; j++)
		{
			struct xgpu_combiner_portion *part;

			synthetic_base(&program);
			synthetic_stage(&program, 0, XGPU_TEXTURE_PROJECT2D, _xgpu_sampler_2d);
			program.fog = 1;
			program.combiner_count = 2;
			program.combiners[1] = program.combiners[0];
			program.combiners[1].c0 = 3;
			program.combiners[1].c1 = 5;
			part = &program.combiners[1].portions[j & 1];
			part->inputs[j >> 1].reg = (unsigned char)i;
			part->inputs[j >> 1].mapping = (unsigned char)j;
			part->inputs[(j >> 1) ^ 1].reg = (unsigned char)(15 - i);
			part->inputs[(j >> 1) ^ 1].alpha = 1;
			part->inputs[(j >> 1) ^ 1].mapping = (unsigned char)(7 - j);
			snprintf(name, sizeof(name), "synthetic-input%d-mapping%d", i, j);
			callback(name, &program);
		}
	}
	/* each output mapping, the dot products, the muxes, the destinations,
	blue to alpha */
	for (i = 0; i < 8; i++)
	{
		for (j = 0; j < 8; j++)
		{
			struct xgpu_combiner_portion *part;
			int portion;

			synthetic_base(&program);
			synthetic_stage(&program, 0, XGPU_TEXTURE_PROJECT2D, _xgpu_sampler_2d);
			synthetic_stage(&program, 1, XGPU_TEXTURE_PROJECT2D, _xgpu_sampler_2d);
			program.combiner_count = 3;
			program.combiners[1] = program.combiners[2] = program.combiners[0];
			program.mux_msb = j & 1;
			for (portion = 0; portion < 2; portion++)
			{
				part = &program.combiners[1].portions[portion];
				part->output_mapping = (unsigned char)i;
				part->ab_dot = !portion && (j & 2);
				part->cd_dot = !portion && (j & 4);
				part->mux = (j & 1) || portion;
				part->ab_blue_to_alpha = !portion && (j & 2);
				part->cd_blue_to_alpha = !portion && (j & 4);
				part->ab = (unsigned char)(4 + ((i + j) % 12));
				part->cd = (unsigned char)(4 + ((i + 2 * j + 3) % 12));
				part->sum = (unsigned char)(4 + ((3 * i + j + 5) % 12));
			}
			for (portion = 0; portion < 2; portion++)
			{
				part = &program.combiners[2].portions[portion];
				part->inputs[0].reg = XGPU_REGISTER_V0;
				part->inputs[1].reg = XGPU_REGISTER_V1;
				part->inputs[2].reg = XGPU_REGISTER_R0;
				part->inputs[3].reg = XGPU_REGISTER_R1;
				part->ab = XGPU_REGISTER_R0;
				part->sum = XGPU_REGISTER_R1;
			}
			snprintf(name, sizeof(name), "synthetic-output%d-flags%d", i, j);
			callback(name, &program);
		}
	}
	/* the final combiner: each input on each register, inverted or not, the
	sum's and product's */
	for (i = 0; i < 16; i++)
	{
		synthetic_base(&program);
		synthetic_stage(&program, 0, XGPU_TEXTURE_PROJECT2D, _xgpu_sampler_2d);
		program.fog = 1;
		program.fog_mode = D3DFOG_EXP;
		for (j = 0; j < 7; j++)
		{
			program.final.inputs[j].reg = (unsigned char)((i + j) & 15);
			/* (E and F make the product: neither reads it nor the sum) */
			if ((j == 4 || j == 5) && program.final.inputs[j].reg >= XGPU_REGISTER_V1R0_SUM)
				program.final.inputs[j].reg = XGPU_REGISTER_V1;
			program.final.inputs[j].alpha = (i + j) % 3 == 0;
			program.final.inputs[j].mapping = (i + j) & 1 ? XGPU_MAP_UNSIGNED_INVERT : XGPU_MAP_UNSIGNED_IDENTITY;
		}
		program.final.clamp_sum = i & 1;
		program.final.invert_v1 = (i >> 1) & 1;
		program.final.invert_r0 = (i >> 2) & 1;
		snprintf(name, sizeof(name), "synthetic-final%d", i);
		callback(name, &program);
	}
	synthetic_base(&program);
	program.final.present = 0;
	callback("synthetic-final-none", &program);
	/* fog, the alpha test, the draw's features */
	for (i = 0; i < 4; i++)
	{
		static const unsigned char modes[] = { D3DFOG_NONE, D3DFOG_EXP, D3DFOG_EXP2, D3DFOG_LINEAR };

		synthetic_base(&program);
		program.final.inputs[1].reg = XGPU_REGISTER_FOG;
		program.final.inputs[1].alpha = 1;
		program.final.inputs[2].reg = XGPU_REGISTER_FOG;
		program.fog = 1;
		program.fog_mode = modes[i];
		snprintf(name, sizeof(name), "synthetic-fog%d", modes[i]);
		callback(name, &program);
	}
	for (i = D3DCMP_NEVER; i <= D3DCMP_ALWAYS; i++)
	{
		for (j = 0; j < 3; j++)
		{
			synthetic_base(&program);
			synthetic_stage(&program, 0, XGPU_TEXTURE_PROJECT2D, _xgpu_sampler_2d);
			program.alpha_test = (unsigned long)i;
			program.alpha_test_samples = (unsigned char)(j ? 2 * j : 0);
			snprintf(name, sizeof(name), "synthetic-alpha%d-samples%d", i, program.alpha_test_samples);
			callback(name, &program);
		}
	}
	for (i = 1; i <= 4; i++)
	{
		synthetic_base(&program);
		synthetic_stage(&program, 0, XGPU_TEXTURE_PROJECT2D, _xgpu_sampler_2d);
		program.source_blend = (unsigned char)i;
		program.coverage_alpha = i & 1;
		program.point_threshold = i >= 3;
		program.count_samples = i & 1;
		snprintf(name, sizeof(name), "synthetic-features%d", i);
		callback(name, &program);
	}
	for (i = 0; i < 6; i++)
	{
		synthetic_base(&program);
		synthetic_stage(&program, 0, XGPU_TEXTURE_PROJECT2D, i == 5 ? _xgpu_sampler_cube : _xgpu_sampler_2d);
		program.final.inputs[1].reg = XGPU_REGISTER_V0;
		program.final.inputs[2].reg = XGPU_REGISTER_V0;
		program.final.inputs[2].alpha = 1;
		program.per_pixel_lighting = (unsigned char)(i < 4 ? 1 + (i & 1) : 0);
		program.dynamic_lights = (unsigned char)(i < 2 ? 0 : i < 4 ? XGPU_DYNAMIC_LIGHTS_MODEL : XGPU_DYNAMIC_LIGHTS_ENVIRONMENT);
		snprintf(name, sizeof(name), "synthetic-lighting%d", i);
		callback(name, &program);
	}
}

/* each MAC and ILU operation, each operand file, relative constants, each
output, the position read back, the clip position captured */
static void synthetic_vertex_programs(synthetic_vertex_callback callback)
{
	struct xgpu_vertex_program program;
	char name[64];
	int variant, i;

	for (variant = 0; variant < 2; variant++)
	{
		memset(&program, 0, sizeof(program));
		for (i = 0; i < 24; i++)
		{
			struct xgpu_vertex_instruction *instruction = &program.instructions[i];
			int k;

			instruction->mac = (unsigned char)(i % 15);
			instruction->ilu = (unsigned char)((i + variant) % 9);
			instruction->a.file = (unsigned char)(1 + i % 3);
			instruction->b.file = (unsigned char)(1 + (i + 1) % 3);
			instruction->c.file = (unsigned char)((i + 2) % 4);
			instruction->a.index = (unsigned char)(i % 3 == 0 ? (i % 13) : i % 3 == 1 ? i % 16 : 40 + i);
			instruction->b.index = (unsigned char)((i + 1) % 3 == 0 ? (i + 5) % 13 : (i + 1) % 3 == 1 ? (i + 3) % 16 : 90 + i);
			instruction->c.index = (unsigned char)((i + 2) % 4 == 1 ? (i + 7) % 13 : (i + 2) % 4 == 2 ? (i + 1) % 16 : 100 + i);
			for (k = 0; k < 4; k++)
			{
				instruction->a.swizzle[k] = (unsigned char)((i + k) & 3);
				instruction->b.swizzle[k] = (unsigned char)((i * 3 + k) & 3);
				instruction->c.swizzle[k] = (unsigned char)((i + 2 * k) & 3);
			}
			instruction->a.negate = i & 1;
			instruction->c.negate = (i >> 1) & 1;
			instruction->relative = variant && (i % 5 == 0);
			instruction->mac_mask = (unsigned char)(1 + (i * 7) % 15);
			instruction->mac_temporary = (unsigned char)((i * 5) % 13);
			instruction->ilu_mask = (unsigned char)(1 + (i * 11) % 15);
			instruction->ilu_temporary = instruction->mac != XGPU_MAC_NOP ? 1 : (unsigned char)((i * 3) % 13);
			instruction->output_mask = (unsigned char)(1 + (i * 13) % 15);
			instruction->output_is_register = 1;
			instruction->output = (unsigned char)(i % 12);
			instruction->output_from_ilu = (i & 2) != 0;
			instruction->writes_output = (instruction->output_from_ilu ? instruction->ilu : instruction->mac) != 0;
		}
		/* the screen conversion: the position from r12, its w's reciprocal */
		program.instructions[24].mac = XGPU_MAC_MUL;
		program.instructions[24].a.file = XGPU_VERTEX_TEMPORARY;
		program.instructions[24].a.index = XGPU_VERTEX_POSITION;
		program.instructions[24].b.file = XGPU_VERTEX_CONSTANT;
		program.instructions[24].b.index = 58;
		program.instructions[24].ilu = XGPU_ILU_RCC;
		program.instructions[24].c.file = XGPU_VERTEX_TEMPORARY;
		program.instructions[24].c.index = XGPU_VERTEX_POSITION;
		for (i = 0; i < 4; i++)
		{
			program.instructions[24].a.swizzle[i] = program.instructions[24].b.swizzle[i] = (unsigned char)i;
			program.instructions[24].c.swizzle[i] = 3;
		}
		program.instructions[24].mac_mask = 0xe;
		program.instructions[24].mac_temporary = XGPU_VERTEX_POSITION;
		program.instructions[24].ilu_mask = 1;
		program.instructions[24].ilu_temporary = XGPU_VERTEX_POSITION;
		program.instructions[24].captures_clip = (unsigned char)variant;
		program.instructions[24].last = 1;
		program.instruction_count = 25;
		program.projects = (unsigned char)variant;
		snprintf(name, sizeof(name), "synthetic-vertex%d", variant);
		callback(name, &program);
	}
}
