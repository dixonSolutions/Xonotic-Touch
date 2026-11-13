// Godnotic: compile one GLSL permutation ahead of its first draw
// (touch_preload replays what an earlier game compiled late). Included at
// the end of gl_rmain.c.

qbool TouchGLSL_Precompile(unsigned int mode, unsigned long long permutation)
{
	r_glsl_permutation_t *p;
	if (mode >= SHADERMODE_COUNT || (vid.renderpath != RENDERPATH_GL32 && vid.renderpath != RENDERPATH_GLES2))
		return false;
	p = R_GLSL_FindPermutation(mode, (uint64_t)permutation);
	if (p->compiled)
		return false;
	R_GLSL_CompilePermutation(p, mode, (uint64_t)permutation);
	// the compile bound its program: the next draw binds its own again
	r_glsl_permutation = NULL;
	CHECKGLERROR
	qglUseProgram(0);CHECKGLERROR
	return true;
}
