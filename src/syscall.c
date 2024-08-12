/*
 * rv32emu is freely redistributable under the MIT License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include "softfloat.h"

#include "riscv.h"
#include "riscv_private.h"
#include "utils.h"

#define PREALLOC_SIZE 4096

/* newlib is a portable (not RISC-V specific) C library, which implements
 * printf(3) and other functions described in C standards. Some system calls
 * should be provided in conjunction with newlib.
 *
 * system call: name, number
 */
/* clang-format off */
#define SUPPORTED_SYSCALLS           \
    _(writeint,              1)      \
    _(writefloat,            2)      \
    _(writestring,           4)      \
    _(readint,               5)      \
    _(readfloat,             6)      \
    _(readstring,            8)      \
    _(exit,                 10)      \
    _(writechar,            11)      \
    _(readchar,             12)      \
    _(writeinthex,          34)      \
    _(close,                57)      \
    _(lseek,                62)      \
    _(read,                 63)      \
    _(write,                64)      \
    _(fstat,                80)      \
    _(exit2,                93)      \
    _(gettimeofday,         169)     \
    _(brk,                  214)     \
    _(clock_gettime,        403)     \
    _(open,                 1024)    \
    IIF(RV32_HAS(SDL))(              \
        _(draw_frame,       0xBEEF)  \
        _(setup_queue,      0xC0DE)  \
        _(submit_queue,     0xFEED)  \
        _(setup_audio,      0xBABE)  \
        _(control_audio,    0xD00D), \
    )
/* clang-format on */

enum {
#define _(name, number) SYS_##name = number,
    SUPPORTED_SYSCALLS
#undef _
};

enum {
    O_RDONLY = 0,
    O_WRONLY = 1,
    O_RDWR = 2,
    O_ACCMODE = 3,
};

static int find_free_fd(vm_attr_t *attr)
{
    for (int i = 3;; ++i) {
        map_iter_t it;
        map_find(attr->fd_map, &it, &i);
        if (map_at_end(attr->fd_map, &it))
            return i;
    }
}

static const char *get_mode_str(uint32_t flags, uint32_t mode UNUSED)
{
    switch (flags & O_ACCMODE) {
    case O_RDONLY:
        return "rb";
    case O_WRONLY:
        return "wb";
    case O_RDWR:
        return "a+";
    default:
        return NULL;
    }
}

static uint8_t tmp[PREALLOC_SIZE];
static void syscall_writestring(riscv_t *rv) {
    vm_attr_t *attr = PRIV(rv);

    riscv_word_t addr = rv_get_reg(rv, rv_reg_a0);
    
    uint32_t total_read = 0;

    while (1) {
        memory_read(attr->mem, tmp, addr + total_read, 1);
        char c = tmp[0];

        if (c == '\0') {
            break;
        }

        putchar(c);

        total_read++;
    }
}

static void syscall_writechar(riscv_t *rv) {
    vm_attr_t *attr = PRIV(rv);

    riscv_word_t addr = rv_get_reg(rv, rv_reg_a0);
    
    uint32_t total_read = 0;

    while (1) {
        memory_read(attr->mem, tmp, addr + total_read, 1);
        char c = tmp[0];
        printf("%c", c);
        if (c == '\0') {
            break;
        }

        putchar(c);

        total_read++;
    }
}

static void syscall_writeint(riscv_t *rv)
{
    /* _write(value) */
    riscv_word_t value = rv_get_reg(rv, rv_reg_a0); // O valor a ser escrito está em a0

    // Escreve o valor no stdout
    size_t written = printf("%d", value);
    if (written <= 0) { // Verifica se houve erro na escrita
        rv_set_reg(rv, rv_reg_a0, -1);
        return;
    }

    rv_set_reg(rv, rv_reg_a0, written); // Retorna o número de bytes escritos
}

static void syscall_writeinthex(riscv_t *rv)
{
    /* _writeinthex(value) */
    riscv_word_t value = rv_get_reg(rv, rv_reg_a0); // O valor a ser escrito está em a0

    // Escreve o valor no stdout em formato hexadecimal
    size_t written = printf("0x%08x", (unsigned int)value); // Imprime em formato hexadecimal com preenchimento de zeros à esquerda

    if (written <= 0) { // Verifica se houve erro na escrita
        rv_set_reg(rv, rv_reg_a0, -1);
        return;
    }

    rv_set_reg(rv, rv_reg_a0, written); // Retorna o número de bytes escritos
}

static void syscall_writefloat(riscv_t *rv) {
    assert(rv);

    // Lê o valor do registrador fa0 (usando F[a0]/F[10])
    riscv_float_t float_value = rv_get_freg(rv, rv_reg_fa0);

    float value;

    // Converte o valor do formato riscv_float_t para float
    memcpy(&value, &float_value, sizeof(float));

    // Escreve o valor no stdout
    size_t written = printf("%.1f", value);
    if (written <= 0) { // Verifica se houve erro na escrita
        riscv_float_t error_value = i32_to_f32(-1);
        rv_set_freg(rv, rv_reg_fa0, error_value);
        return;
    }
}



static void syscall_readint(riscv_t *rv) {
  vm_attr_t *attr = PRIV(rv); // Obtém um ponteiro para um atributo privado da estrutura riscv_t

  /* _read(fd, buf, count); */
  uint32_t fd = rv_get_reg(rv, 0);// Lê o valor do registrador a0 (descritor de arquivo)

  /* lookup the file */
  map_iter_t it; // Cria um iterador para buscar o arquivo no mapa de arquivos
  map_find(attr->fd_map, &it, &fd); // Busca o arquivo correspondente ao descritor
  if (map_at_end(attr->fd_map, &it)) { // Se não encontrou o arquivo ??
    /* error */
    rv_set_reg(rv, rv_reg_a0, -1);
    return;
  }

  // Se encontrou o arquivo, pega o ponteiro para ele
  FILE *handle = map_iter_value(&it, FILE *); // Ponteiro para o arquivo aberto

  // Garante que o buffer tem tamanho suficiente para guardar todos os inteiros
  char line[sizeof(int) + 1]; // Buffer para armazenar a linha lida do arquivo

  // Lê dados do arquivo para o buffer
  int value;
  size_t bytes_read = fscanf(handle, "%d", &value);
    //TODO - Verificar como indicar erro na leitura de inteiros (Exceção?)
    #if 0
    // Verifica se houve erro ou leitura incompleta
    if (bytes_read == sizeof(int)) {
        /* error */
        rv_set_reg(rv, rv_reg_a0, -1);
        return;
    }
    #endif 

  rv_set_reg(rv, rv_reg_a0, value); // Retorna o total de bytes lidos
}

static void syscall_readfloat(riscv_t *rv) {
    vm_attr_t *attr = PRIV(rv); // Obtains a pointer to a private attribute of the riscv_t structure

    // Read the file descriptor from the integer register a0
    softfloat_float32_t fd = rv_get_freg(rv, rv_reg_fa0);

    // Lookup the file associated with the file descriptor
    map_iter_t it;
    map_find(attr->fd_map, &it, &fd);

    if (map_at_end(attr->fd_map, &it)) {
        // Descritor de arquivo não encontrado; define o código de erro como um valor de ponto flutuante
        softfloat_float32_t error_value = i32_to_f32(-1);
        rv_set_freg(rv, rv_reg_fa0, error_value);
        return;
    }

    // Get the file pointer from the map
    FILE *handle = map_iter_value(&it, FILE *);

    // Read a float value from the file
    float value;
    size_t bytes_read = fscanf(handle, "%f", &value);

    // Convert the float to the appropriate floating-point type and store it in register f0
    riscv_float_t float_value;
    memcpy(&float_value, &value, sizeof(float));
    rv_set_freg(rv, rv_reg_fa0, float_value);
}

static void syscall_exit(riscv_t *rv)
{
    /* simply halt cpu and save exit code.
     * the application decides the usage of exit code
     */
    rv_halt(rv);

    vm_attr_t *attr = PRIV(rv);
    attr->exit_code = 0;
}

static void syscall_write(riscv_t *rv)
{
    vm_attr_t *attr = PRIV(rv);

    /* _write(fd, buffer, count) */
    riscv_word_t fd = rv_get_reg(rv, rv_reg_a0);
    riscv_word_t buffer = rv_get_reg(rv, rv_reg_a1);
    riscv_word_t count = rv_get_reg(rv, rv_reg_a2);


    /* lookup the file descriptor */
    map_iter_t it;
    map_find(attr->fd_map, &it, &fd);
    uint32_t total_write = 0;

    while (count > PREALLOC_SIZE) {
        memory_read(attr->mem, tmp, buffer + total_write, PREALLOC_SIZE);
        if (!map_at_end(attr->fd_map, &it)) {
            /* write out the data */
            FILE *handle = map_iter_value(&it, FILE *);
            size_t written = fwrite(tmp, 1, PREALLOC_SIZE, handle);
            if (written != PREALLOC_SIZE && ferror(handle))
                goto error_handler;
            total_write += written;
            count -= PREALLOC_SIZE;
        } else
            goto error_handler;
    }

    memory_read(attr->mem, tmp, buffer + total_write, count);
    if (!map_at_end(attr->fd_map, &it)) {
        /* write out the data */
        FILE *handle = map_iter_value(&it, FILE *);
        size_t written = fwrite(tmp, 1, count, handle);
        if (written != count && ferror(handle))
            goto error_handler;
        total_write += written;
    } else
        goto error_handler;
    assert(total_write == rv_get_reg(rv, rv_reg_a2));

    /* return number of bytes written */
    rv_set_reg(rv, rv_reg_a0, total_write);
    return;

    /* read the string being printed */
error_handler:
    /* error */
    rv_set_reg(rv, rv_reg_a0, -1);
}


static void syscall_exit2(riscv_t *rv)
{
    /* simply halt cpu and save exit code.
     * the application decides the usage of exit code
     */
    rv_halt(rv);

    vm_attr_t *attr = PRIV(rv);
    attr->exit_code = rv_get_reg(rv, rv_reg_a0);
}

/* brk(increment)
 * Note:
 *   - 8 byte alignment for malloc chunks
 *   - 4 KiB aligned for sbrk blocks
 */
static void syscall_brk(riscv_t *rv)
{
    vm_attr_t *attr = PRIV(rv);

    /* get the increment parameter */
    riscv_word_t increment = rv_get_reg(rv, rv_reg_a0);
    if (increment)
        attr->break_addr = increment;

    /* return new break address */
    rv_set_reg(rv, rv_reg_a0, attr->break_addr);
}

static void syscall_gettimeofday(riscv_t *rv)
{
    /* get the parameters */
    riscv_word_t tv = rv_get_reg(rv, rv_reg_a0);
    riscv_word_t tz = rv_get_reg(rv, rv_reg_a1);

    /* return the clock time */
    if (tv) {
        struct timeval tv_s;
        rv_gettimeofday(&tv_s);
        memory_write_w(tv + 0, (const uint8_t *) &tv_s.tv_sec);
        memory_write_w(tv + 8, (const uint8_t *) &tv_s.tv_usec);
    }

    if (tz) {
        /* FIXME: This parameter is ignored by the syscall handler in newlib. */
    }

    /* success */
    rv_set_reg(rv, rv_reg_a0, 0);
}

static void syscall_clock_gettime(riscv_t *rv)
{
    /* get the parameters */
    riscv_word_t id = rv_get_reg(rv, rv_reg_a0);
    riscv_word_t tp = rv_get_reg(rv, rv_reg_a1);

    switch (id) {
    case CLOCK_REALTIME:
#ifdef CLOCK_MONOTONIC
    case CLOCK_MONOTONIC:
#endif
        break;
    default:
        rv_set_reg(rv, rv_reg_a0, -1);
        return;
    }

    if (tp) {
        struct timespec tp_s;
        rv_clock_gettime(&tp_s);
        memory_write_w(tp + 0, (const uint8_t *) &tp_s.tv_sec);
        memory_write_w(tp + 8, (const uint8_t *) &tp_s.tv_nsec);
    }

    /* success */
    rv_set_reg(rv, rv_reg_a0, 0);
}

static void syscall_close(riscv_t *rv)
{
    vm_attr_t *attr = PRIV(rv);

    /* _close(fd); */
    uint32_t fd = rv_get_reg(rv, rv_reg_a0);

    if (fd >= 3) { /* lookup the file descriptor */
        map_iter_t it;
        map_find(attr->fd_map, &it, &fd);
        if (!map_at_end(attr->fd_map, &it)) {
            if (fclose(map_iter_value(&it, FILE *))) {
                /* error */
                rv_set_reg(rv, rv_reg_a0, -1);
                return;
            }
            map_erase(attr->fd_map, &it);

            /* success */
            rv_set_reg(rv, rv_reg_a0, 0);
        }
    }

    /* success */
    rv_set_reg(rv, rv_reg_a0, 0);
}

/* lseek() repositions the file offset of the open file description associated
 * with the file descriptor fd to the argument offset according to the
 * directive whence.
 */
static void syscall_lseek(riscv_t *rv)
{
    vm_attr_t *attr = PRIV(rv);

    /* _lseek(fd, offset, whence); */
    uint32_t fd = rv_get_reg(rv, rv_reg_a0);
    uint32_t offset = rv_get_reg(rv, rv_reg_a1);
    uint32_t whence = rv_get_reg(rv, rv_reg_a2);

    /* find the file descriptor */
    map_iter_t it;
    map_find(attr->fd_map, &it, &fd);
    if (map_at_end(attr->fd_map, &it)) {
        /* error */
        rv_set_reg(rv, rv_reg_a0, -1);
        return;
    }

    FILE *handle = map_iter_value(&it, FILE *);
    if (fseek(handle, offset, whence)) {
        /* error */
        rv_set_reg(rv, rv_reg_a0, -1);
        return;
    }

    /* success */
    rv_set_reg(rv, rv_reg_a0, 0);
}

static void syscall_read(riscv_t *rv)
{
    vm_attr_t *attr = PRIV(rv);

    /* _read(fd, buf, count); */
    uint32_t fd = rv_get_reg(rv, rv_reg_a0);
    uint32_t buf = rv_get_reg(rv, rv_reg_a1);
    uint32_t count = rv_get_reg(rv, rv_reg_a2);

    /* lookup the file */
    map_iter_t it;
    map_find(attr->fd_map, &it, &fd);
    if (map_at_end(attr->fd_map, &it)) {
        /* error */
        rv_set_reg(rv, rv_reg_a0, -1);
        return;
    }

    FILE *handle = map_iter_value(&it, FILE *);
    uint32_t total_read = 0;
    /* read the file into runtime memory */

    while (count > PREALLOC_SIZE) {
        size_t r = fread(tmp, 1, PREALLOC_SIZE, handle);
        memory_write(attr->mem, buf + total_read, tmp, r);
        count -= r;
        total_read += r;
        if (r != PREALLOC_SIZE)
            break;
    }
    size_t r = fread(tmp, 1, count, handle);
    memory_write(attr->mem, buf + total_read, tmp, r);
    total_read += r;
    if (total_read != rv_get_reg(rv, rv_reg_a2) && ferror(handle)) {
        /* error */
        rv_set_reg(rv, rv_reg_a0, -1);
        return;
    }
    /* success */
    rv_set_reg(rv, rv_reg_a0, total_read);
}

static void syscall_readstring(riscv_t *rv)
{
    vm_attr_t *attr = PRIV(rv);

    uint32_t buf = rv_get_reg(rv, rv_reg_a0);
    uint32_t max_chars = rv_get_reg(rv, rv_reg_a1);

    char myString[PREALLOC_SIZE];
    scanf("%[^\n]s", myString);
    // Quantos caracteres foram lidos (excluindo o '\0').
    uint32_t total_read = strlen(myString);

    if (total_read > max_chars - 1) {
        total_read = max_chars - 1;
    }

    // Escrevemos a string na memória usando memory_write.
    for (uint32_t i = 0; i < total_read; ++i) {
        memory_write(attr->mem, buf + i, &myString[i], 1);
    }
    // Adicionamos o terminador nulo '\0' ao final da string na memória.
    memory_write(attr->mem, buf + total_read, (const uint8_t *) "\0", 1);

    // Retornamos o número total de caracteres lidos (excluindo o '\0').
    rv_set_reg(rv, rv_reg_a0, total_read);
}

static void syscall_readchar(riscv_t *rv)
{
    vm_attr_t *attr = PRIV(rv);

    uint32_t buf = rv_get_reg(rv, rv_reg_a0);
    uint32_t max_chars = rv_get_reg(rv, rv_reg_a1);

    // Verificar se o buffer tem espaço para pelo menos um caractere e o terminador nulo.
    if (max_chars < 2) {
        rv_set_reg(rv, rv_reg_a0, 0); // Retornar 0 caracteres lidos se o buffer for muito pequeno.
        return;
    }

    // Ler um único caractere da entrada padrão.
    char ch;
    scanf("%c", &ch);

    // Escrever o caractere lido na memória.
    memory_write(attr->mem, buf, &ch, 1);

    // Adicionar o terminador nulo '\0' ao final do caractere na memória.
    char null_terminator = '\0';
    memory_write(attr->mem, buf + 1, &null_terminator, 1);

    // Retornar o número total de caracteres lidos (1, excluindo o '\0').
    rv_set_reg(rv, rv_reg_a0, 1);
}

static void syscall_fstat(riscv_t *rv UNUSED)
{
    /* FIXME: fill real implementation */
}

static void syscall_open(riscv_t *rv)
{
    vm_attr_t *attr = PRIV(rv);

    /* _open(name, flags, mode); */
    uint32_t name = rv_get_reg(rv, rv_reg_a0);
    uint32_t flags = rv_get_reg(rv, rv_reg_a1);
    uint32_t mode = rv_get_reg(rv, rv_reg_a2);

    /* read name from runtime memory */
    const size_t name_len = strlen((char *) attr->mem->mem_base + name);
    char *name_str = malloc(name_len + 1);
    assert(name_str);
    name_str[name_len] = '\0';
    memory_read(attr->mem, (uint8_t *) name_str, name, name_len);

    /* open the file */
    const char *mode_str = get_mode_str(flags, mode);
    if (!mode_str) {
        rv_set_reg(rv, rv_reg_a0, -1);
        return;
    }

    FILE *handle = fopen(name_str, mode_str);
    if (!handle) {
        rv_set_reg(rv, rv_reg_a0, -1);
        return;
    }

    free(name_str);

    const int fd = find_free_fd(attr); /* find a free file descriptor */

    /* insert into the file descriptor map */
    map_insert(attr->fd_map, (void *) &fd, &handle);

    /* return the file descriptor */
    rv_set_reg(rv, rv_reg_a0, fd);
}

#if RV32_HAS(SDL)
extern void syscall_draw_frame(riscv_t *rv);
extern void syscall_setup_queue(riscv_t *rv);
extern void syscall_submit_queue(riscv_t *rv);
extern void syscall_setup_audio(riscv_t *rv);
extern void syscall_control_audio(riscv_t *rv);
#endif

void syscall_handler(riscv_t *rv)
{
    /* get the syscall number */
    riscv_word_t syscall = rv_get_reg(rv, rv_reg_a7);

    switch (syscall) { /* dispatch system call */
#define _(name, number)     \
    case SYS_##name:        \
        syscall_##name(rv); \
        break;
        SUPPORTED_SYSCALLS
#undef _
    default:
        fprintf(stderr, "unknown syscall %d\n", (int) syscall);
        break;
    }

    /* save return code.
     * the application decides the usage of the return code
     */
    vm_attr_t *attr = PRIV(rv);
    attr->error = rv_get_reg(rv, rv_reg_a0);
}
