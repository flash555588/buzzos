#ifndef BUZZOS_BASM_H
#define BUZZOS_BASM_H

/*
 * Assemble a complete source buffer and write a standalone i386 ELF.
 * Returns zero on success. Diagnostics are printed with the supplied name.
 */
int basm_compile_source(const char *name, const char *source, int source_len,
                        const char *output_path, int verbose);

#endif
