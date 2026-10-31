/* tcc_fixup: the compiler launcher cmake/tcc.cmake puts in front of tcc.
 *
 * CMake runs `tcc_fixup [ccache] tcc ... -o <object> -c <source>`. This runs
 * the rest of that command, then corrects the object file tcc wrote.
 *
 * In an x86-64 ELF object, a relocation carries its whole addend, and the
 * linker writes the address it works out over the bytes the relocation points
 * at. tcc's own linker adds it to those bytes instead, and newer tcc, in its
 * debug information (DWARF) and unwind tables, leaves part of each address
 * there: where in the file the function starts. Every other linker reads the
 * relocation alone, and puts every function of a file where its first one is:
 * gdb then stops in the wrong function, and GNU ld refuses the unwind tables.
 * So this moves those bytes into the relocation, where every linker looks.
 * tcc 0.9.27 writes neither, and its objects come out as they went in. */

#include <elf.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* How many bytes a relocation of this type patches, and whether they are a
   signed number. 0 for the types tcc does not write in these sections. */
static int width(unsigned type, int *is_signed)
{
    *is_signed = 1;
    switch (type) {
    case R_X86_64_64:
    case R_X86_64_PC64:
        return 8;
    case R_X86_64_PC32:
    case R_X86_64_32S:
        return 4;
    case R_X86_64_32:
        *is_signed = 0;
        return 4;
    default:
        return 0;
    }
}

static Elf64_Shdr section(const unsigned char *d, const Elf64_Ehdr *eh, unsigned i)
{
    Elf64_Shdr s;
    memcpy(&s, d + eh->e_shoff + (uint64_t)i * sizeof s, sizeof s);
    return s;
}

/* A section's bytes are all in the file. */
static int inside(const Elf64_Shdr *s, uint64_t size)
{
    return s->sh_offset <= size && s->sh_size <= size - s->sh_offset;
}

/* Reads a whole file; NULL, having said why, if it cannot. */
static unsigned char *slurp(const char *path, long *size)
{
    FILE *f = fopen(path, "rb");
    unsigned char *d = NULL;

    if (f && fseek(f, 0, SEEK_END) == 0 && (*size = ftell(f)) >= 0 && fseek(f, 0, SEEK_SET) == 0
        && (d = malloc(*size ? *size : 1)) != NULL && fread(d, 1, *size, f) != (size_t)*size) {
        free(d);
        d = NULL;
    }
    if (!d)
        perror(path);
    if (f)
        fclose(f);
    return d;
}

static int fix(const char *path)
{
    unsigned char *d;
    long size;
    Elf64_Ehdr eh;
    Elf64_Shdr names;
    unsigned i, j;
    int changed = 0, failed = 0;

    if (!(d = slurp(path, &size)))
        return 1;

    /* Only what tcc writes for this machine; anything else stays as it is. */
    if ((size_t)size < sizeof eh)
        goto done;
    memcpy(&eh, d, sizeof eh);
    if (memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0 || eh.e_ident[EI_CLASS] != ELFCLASS64
        || eh.e_ident[EI_DATA] != ELFDATA2LSB || eh.e_type != ET_REL || eh.e_machine != EM_X86_64
        || eh.e_shentsize != sizeof(Elf64_Shdr) || eh.e_shoff > (uint64_t)size
        || eh.e_shnum > ((uint64_t)size - eh.e_shoff) / sizeof(Elf64_Shdr) || eh.e_shstrndx >= eh.e_shnum)
        goto done;
    names = section(d, &eh, eh.e_shstrndx);
    if (!inside(&names, size))
        goto done;

    for (i = 0; i < eh.e_shnum; i++) {
        Elf64_Shdr rels = section(d, &eh, i), target;
        const char *name;

        if (rels.sh_type != SHT_RELA || rels.sh_entsize != sizeof(Elf64_Rela) || rels.sh_info >= eh.e_shnum
            || !inside(&rels, size))
            continue;
        target = section(d, &eh, rels.sh_info);
        if (target.sh_type == SHT_NOBITS || !inside(&target, size) || target.sh_name >= names.sh_size
            || !memchr(d + names.sh_offset + target.sh_name, 0, names.sh_size - target.sh_name))
            continue;
        name = (const char *)d + names.sh_offset + target.sh_name;
        if (strncmp(name, ".debug_", 7) != 0 && strcmp(name, ".eh_frame") != 0)
            continue;

        for (j = 0; j < rels.sh_size / sizeof(Elf64_Rela); j++) {
            unsigned char *at = d + rels.sh_offset + (uint64_t)j * sizeof(Elf64_Rela), *p;
            Elf64_Rela r;
            uint64_t v = 0;
            int n, is_signed, k;

            memcpy(&r, at, sizeof r);
            n = width(ELF64_R_TYPE(r.r_info), &is_signed);
            if (n == 0 || target.sh_size < (uint64_t)n || r.r_offset > target.sh_size - n)
                continue;
            p = d + target.sh_offset + r.r_offset;
            for (k = n - 1; k >= 0; k--)
                v = v << 8 | p[k];
            if (v == 0)
                continue;
            r.r_addend += n == 4 && is_signed ? (int64_t)(int32_t)(uint32_t)v : (int64_t)v;
            memcpy(at, &r, sizeof r);
            memset(p, 0, n);
            changed = 1;
        }
    }

    /* A new file, renamed over the old one: never half an object, and never
       ccache's copy, if ccache handed over a hard link to it. */
    if (changed) {
        char *tmp = malloc(strlen(path) + sizeof ".fixup");
        FILE *f = NULL;

        if (tmp) {
            sprintf(tmp, "%s.fixup", path);
            f = fopen(tmp, "wb");
        }
        if (!f || fwrite(d, 1, size, f) != (size_t)size) {
            failed = 1;
            if (f)
                fclose(f);
        } else if (fclose(f) != 0 || rename(tmp, path) != 0) {
            failed = 1;
        }
        if (failed) {
            perror(tmp ? tmp : path);
            if (tmp)
                remove(tmp);
        }
        free(tmp);
    }
done:
    free(d);
    return failed;
}

int main(int argc, char **argv)
{
    const char *object = NULL;
    pid_t pid;
    int status, i;

    if (argc < 2) {
        fprintf(stderr, "usage: tcc_fixup <compiler> <arguments>...\n");
        return 2;
    }
    pid = fork();
    if (pid == 0) {
        execvp(argv[1], argv + 1);
        perror(argv[1]);
        _exit(127);
    }
    if (pid < 0) {
        perror("tcc_fixup");
        return 1;
    }
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            perror("tcc_fixup");
            return 1;
        }
    }
    if (!WIFEXITED(status))
        return 128 + WTERMSIG(status);
    if (WEXITSTATUS(status) != 0)
        return WEXITSTATUS(status);

    for (i = 2; i + 1 < argc; i++)
        if (strcmp(argv[i], "-o") == 0)
            object = argv[i + 1];
    return object ? fix(object) : 0;
}
