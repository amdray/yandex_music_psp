#ifndef YM_SERVICES_TOKEN_LOADER_H
#define YM_SERVICES_TOKEN_LOADER_H

/* Supported application buffer size, including the terminating NUL. */
enum { TOKEN_LOADER_CAPACITY = 256 };

/* Call once from main before starting consumers. The lock lives until exit. */
int token_loader_init(void);

/* Serialized file/cache operations: 0 on success, nonzero on failure.
 * PSP errors are preserved; invalid input/format or no write progress is -1.
 * read never truncates; save accepts at most CAPACITY - 1 token bytes.
 * A failed file modification invalidates the cache.
 */
int token_loader_read(char *out_token, int out_size);
int token_loader_save(const char *token);
int token_loader_remove(void);

#endif
