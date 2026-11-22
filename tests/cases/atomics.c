/* Sized __atomic_*_N and __sync_*_N builtins return an unsigned integer of
 * that size, whatever the pointer points to (checked like gnu.c). */
volatile void *atomic_p;
_Static_assert(__builtin_types_compatible_p(__typeof__(__atomic_exchange_1(atomic_p, 0, 0)), unsigned char), "__atomic_exchange_1(atomic_p, 0, 0)");
_Static_assert(__builtin_types_compatible_p(__typeof__(__atomic_load_1(atomic_p, 0)), unsigned char), "__atomic_load_1(atomic_p, 0)");
_Static_assert(__builtin_types_compatible_p(__typeof__(__sync_fetch_and_add_1(atomic_p, 0)), unsigned char), "__sync_fetch_and_add_1(atomic_p, 0)");
_Static_assert(__builtin_types_compatible_p(__typeof__(__atomic_fetch_or_1(atomic_p, 1, 0)), unsigned char), "__atomic_fetch_or_1(atomic_p, 1, 0)");
_Static_assert(__builtin_types_compatible_p(__typeof__(__atomic_exchange_2(atomic_p, 0, 0)), unsigned short), "__atomic_exchange_2(atomic_p, 0, 0)");
_Static_assert(__builtin_types_compatible_p(__typeof__(__atomic_load_2(atomic_p, 0)), unsigned short), "__atomic_load_2(atomic_p, 0)");
_Static_assert(__builtin_types_compatible_p(__typeof__(__sync_fetch_and_add_2(atomic_p, 0)), unsigned short), "__sync_fetch_and_add_2(atomic_p, 0)");
_Static_assert(__builtin_types_compatible_p(__typeof__(__atomic_fetch_or_2(atomic_p, 1, 0)), unsigned short), "__atomic_fetch_or_2(atomic_p, 1, 0)");
_Static_assert(__builtin_types_compatible_p(__typeof__(__atomic_exchange_4(atomic_p, 0, 0)), unsigned int), "__atomic_exchange_4(atomic_p, 0, 0)");
_Static_assert(__builtin_types_compatible_p(__typeof__(__atomic_load_4(atomic_p, 0)), unsigned int), "__atomic_load_4(atomic_p, 0)");
_Static_assert(__builtin_types_compatible_p(__typeof__(__sync_fetch_and_add_4(atomic_p, 0)), unsigned int), "__sync_fetch_and_add_4(atomic_p, 0)");
_Static_assert(__builtin_types_compatible_p(__typeof__(__atomic_fetch_or_4(atomic_p, 1, 0)), unsigned int), "__atomic_fetch_or_4(atomic_p, 1, 0)");
_Static_assert(__builtin_types_compatible_p(__typeof__(__atomic_exchange_8(atomic_p, 0, 0)), unsigned long), "__atomic_exchange_8(atomic_p, 0, 0)");
_Static_assert(__builtin_types_compatible_p(__typeof__(__atomic_load_8(atomic_p, 0)), unsigned long), "__atomic_load_8(atomic_p, 0)");
_Static_assert(__builtin_types_compatible_p(__typeof__(__sync_fetch_and_add_8(atomic_p, 0)), unsigned long), "__sync_fetch_and_add_8(atomic_p, 0)");
_Static_assert(__builtin_types_compatible_p(__typeof__(__atomic_fetch_or_8(atomic_p, 1, 0)), unsigned long), "__atomic_fetch_or_8(atomic_p, 1, 0)");
_Static_assert(__builtin_types_compatible_p(__typeof__(__atomic_exchange_16(atomic_p, 0, 0)), unsigned __int128), "__atomic_exchange_16(atomic_p, 0, 0)");
_Static_assert(__builtin_types_compatible_p(__typeof__(__atomic_load_16(atomic_p, 0)), unsigned __int128), "__atomic_load_16(atomic_p, 0)");
_Static_assert(__builtin_types_compatible_p(__typeof__(__sync_fetch_and_add_16(atomic_p, 0)), unsigned __int128), "__sync_fetch_and_add_16(atomic_p, 0)");
_Static_assert(__builtin_types_compatible_p(__typeof__(__atomic_fetch_or_16(atomic_p, 1, 0)), unsigned __int128), "__atomic_fetch_or_16(atomic_p, 1, 0)");
void atomic_store(void) { __atomic_store_4(atomic_p, 1, 0); __sync_lock_release_4(atomic_p); }
int atomic_cas(void) { return __sync_bool_compare_and_swap_4(atomic_p, 0, 1); }
