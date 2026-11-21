/* __has_builtin for every builtin that real headers ask about (surveyed
 * from /usr/include, mingw-w64 and the corpus): same answers as the
 * compiler, compared with gcc -E and clang -E. */
#if __has_builtin(__atomic_add_fetch)
Y __atomic_add_fetch
#else
N __atomic_add_fetch
#endif
#if __has_builtin(__atomic_load)
Y __atomic_load
#else
N __atomic_load
#endif
#if __has_builtin(__atomic_load_n)
Y __atomic_load_n
#else
N __atomic_load_n
#endif
#if __has_builtin(__bases)
Y __bases
#else
N __bases
#endif
#if __has_builtin(__builtin_)
Y __builtin_
#else
N __builtin_
#endif
#if __has_builtin(__builtin_addcll)
Y __builtin_addcll
#else
N __builtin_addcll
#endif
#if __has_builtin(__builtin_add_overflow)
Y __builtin_add_overflow
#else
N __builtin_add_overflow
#endif
#if __has_builtin(__builtin_addressof)
Y __builtin_addressof
#else
N __builtin_addressof
#endif
#if __has_builtin(__builtin_alloca)
Y __builtin_alloca
#else
N __builtin_alloca
#endif
#if __has_builtin(__builtin_arm_crypto_vmullp64)
Y __builtin_arm_crypto_vmullp64
#else
N __builtin_arm_crypto_vmullp64
#endif
#if __has_builtin(__builtin_arm_yield)
Y __builtin_arm_yield
#else
N __builtin_arm_yield
#endif
#if __has_builtin(__builtin_assume)
Y __builtin_assume
#else
N __builtin_assume
#endif
#if __has_builtin(__builtin_assume_aligned)
Y __builtin_assume_aligned
#else
N __builtin_assume_aligned
#endif
#if __has_builtin(__builtin_available)
Y __builtin_available
#else
N __builtin_available
#endif
#if __has_builtin(__builtin_bit_cast)
Y __builtin_bit_cast
#else
N __builtin_bit_cast
#endif
#if __has_builtin(__builtin_bitreverse16)
Y __builtin_bitreverse16
#else
N __builtin_bitreverse16
#endif
#if __has_builtin(__builtin_bitreverse32)
Y __builtin_bitreverse32
#else
N __builtin_bitreverse32
#endif
#if __has_builtin(__builtin_bitreverse64)
Y __builtin_bitreverse64
#else
N __builtin_bitreverse64
#endif
#if __has_builtin(__builtin_bitreverse8)
Y __builtin_bitreverse8
#else
N __builtin_bitreverse8
#endif
#if __has_builtin(__builtin_bswap128)
Y __builtin_bswap128
#else
N __builtin_bswap128
#endif
#if __has_builtin(__builtin_bswap16)
Y __builtin_bswap16
#else
N __builtin_bswap16
#endif
#if __has_builtin(__builtin_bswap32)
Y __builtin_bswap32
#else
N __builtin_bswap32
#endif
#if __has_builtin(__builtin_bswap64)
Y __builtin_bswap64
#else
N __builtin_bswap64
#endif
#if __has_builtin(__builtin_clear_padding)
Y __builtin_clear_padding
#else
N __builtin_clear_padding
#endif
#if __has_builtin(__builtin_clz)
Y __builtin_clz
#else
N __builtin_clz
#endif
#if __has_builtin(__builtin_clzg)
Y __builtin_clzg
#else
N __builtin_clzg
#endif
#if __has_builtin(__builtin_clzl)
Y __builtin_clzl
#else
N __builtin_clzl
#endif
#if __has_builtin(__builtin_clzll)
Y __builtin_clzll
#else
N __builtin_clzll
#endif
#if __has_builtin(__builtin_clzs)
Y __builtin_clzs
#else
N __builtin_clzs
#endif
#if __has_builtin(__builtin_common_type)
Y __builtin_common_type
#else
N __builtin_common_type
#endif
#if __has_builtin(__builtin_constant_p)
Y __builtin_constant_p
#else
N __builtin_constant_p
#endif
#if __has_builtin(__builtin_constexpr_diag)
Y __builtin_constexpr_diag
#else
N __builtin_constexpr_diag
#endif
#if __has_builtin(__builtin_copysign)
Y __builtin_copysign
#else
N __builtin_copysign
#endif
#if __has_builtin(__builtin_coro_noop)
Y __builtin_coro_noop
#else
N __builtin_coro_noop
#endif
#if __has_builtin(__builtin_ctz)
Y __builtin_ctz
#else
N __builtin_ctz
#endif
#if __has_builtin(__builtin_ctzg)
Y __builtin_ctzg
#else
N __builtin_ctzg
#endif
#if __has_builtin(__builtin_ctzll)
Y __builtin_ctzll
#else
N __builtin_ctzll
#endif
#if __has_builtin(__builtin_ctzs)
Y __builtin_ctzs
#else
N __builtin_ctzs
#endif
#if __has_builtin(__builtin_debugtrap)
Y __builtin_debugtrap
#else
N __builtin_debugtrap
#endif
#if __has_builtin(__builtin_dx_dot2add)
Y __builtin_dx_dot2add
#else
N __builtin_dx_dot2add
#endif
#if __has_builtin(__builtin_dynamic_object_size)
Y __builtin_dynamic_object_size
#else
N __builtin_dynamic_object_size
#endif
#if __has_builtin(__builtin_elementwise_fshl)
Y __builtin_elementwise_fshl
#else
N __builtin_elementwise_fshl
#endif
#if __has_builtin(__builtin_expect)
Y __builtin_expect
#else
N __builtin_expect
#endif
#if __has_builtin(__builtin_fclose)
Y __builtin_fclose
#else
N __builtin_fclose
#endif
#if __has_builtin(__builtin_frame_address)
Y __builtin_frame_address
#else
N __builtin_frame_address
#endif
#if __has_builtin(__builtin_ia32_bzhi_di)
Y __builtin_ia32_bzhi_di
#else
N __builtin_ia32_bzhi_di
#endif
#if __has_builtin(__builtin_ia32_bzhi_si)
Y __builtin_ia32_bzhi_si
#else
N __builtin_ia32_bzhi_si
#endif
#if __has_builtin(__builtin_ia32_pause)
Y __builtin_ia32_pause
#else
N __builtin_ia32_pause
#endif
#if __has_builtin(__builtin_ia32_pdep_di)
Y __builtin_ia32_pdep_di
#else
N __builtin_ia32_pdep_di
#endif
#if __has_builtin(__builtin_ia32_pdep_si)
Y __builtin_ia32_pdep_si
#else
N __builtin_ia32_pdep_si
#endif
#if __has_builtin(__builtin_ia32_pext_di)
Y __builtin_ia32_pext_di
#else
N __builtin_ia32_pext_di
#endif
#if __has_builtin(__builtin_ia32_pext_si)
Y __builtin_ia32_pext_si
#else
N __builtin_ia32_pext_si
#endif
#if __has_builtin(__builtin_ia32_undef128)
Y __builtin_ia32_undef128
#else
N __builtin_ia32_undef128
#endif
#if __has_builtin(__builtin_is_constant_evaluated)
Y __builtin_is_constant_evaluated
#else
N __builtin_is_constant_evaluated
#endif
#if __has_builtin(__builtin_is_corresponding_member)
Y __builtin_is_corresponding_member
#else
N __builtin_is_corresponding_member
#endif
#if __has_builtin(__builtin_isfinite)
Y __builtin_isfinite
#else
N __builtin_isfinite
#endif
#if __has_builtin(__builtin_is_implicit_lifetime)
Y __builtin_is_implicit_lifetime
#else
N __builtin_is_implicit_lifetime
#endif
#if __has_builtin(__builtin_isinf)
Y __builtin_isinf
#else
N __builtin_isinf
#endif
#if __has_builtin(__builtin_isnan)
Y __builtin_isnan
#else
N __builtin_isnan
#endif
#if __has_builtin(__builtin_is_pointer_interconvertible_with_class)
Y __builtin_is_pointer_interconvertible_with_class
#else
N __builtin_is_pointer_interconvertible_with_class
#endif
#if __has_builtin(__builtin_is_string_literal)
Y __builtin_is_string_literal
#else
N __builtin_is_string_literal
#endif
#if __has_builtin(__builtin_is_structural)
Y __builtin_is_structural
#else
N __builtin_is_structural
#endif
#if __has_builtin(__builtin_is_virtual_base_of)
Y __builtin_is_virtual_base_of
#else
N __builtin_is_virtual_base_of
#endif
#if __has_builtin(__builtin_launder)
Y __builtin_launder
#else
N __builtin_launder
#endif
#if __has_builtin(__builtin_memcpy)
Y __builtin_memcpy
#else
N __builtin_memcpy
#endif
#if __has_builtin(__builtin_mul_overflow)
Y __builtin_mul_overflow
#else
N __builtin_mul_overflow
#endif
#if __has_builtin(__builtin_nansf128)
Y __builtin_nansf128
#else
N __builtin_nansf128
#endif
#if __has_builtin(__builtin_nansq)
Y __builtin_nansq
#else
N __builtin_nansq
#endif
#if __has_builtin(__builtin_nondeterministic_value)
Y __builtin_nondeterministic_value
#else
N __builtin_nondeterministic_value
#endif
#if __has_builtin(__builtin_nontemporal_store)
Y __builtin_nontemporal_store
#else
N __builtin_nontemporal_store
#endif
#if __has_builtin(__builtin_observable_checkpoint)
Y __builtin_observable_checkpoint
#else
N __builtin_observable_checkpoint
#endif
#if __has_builtin(__builtin_offsetof)
Y __builtin_offsetof
#else
N __builtin_offsetof
#endif
#if __has_builtin(__builtin_operator_delete)
Y __builtin_operator_delete
#else
N __builtin_operator_delete
#endif
#if __has_builtin(__builtin_operator_new)
Y __builtin_operator_new
#else
N __builtin_operator_new
#endif
#if __has_builtin(__builtin_popcount)
Y __builtin_popcount
#else
N __builtin_popcount
#endif
#if __has_builtin(__builtin_popcountg)
Y __builtin_popcountg
#else
N __builtin_popcountg
#endif
#if __has_builtin(__builtin_popcountll)
Y __builtin_popcountll
#else
N __builtin_popcountll
#endif
#if __has_builtin(__builtin_prefetch)
Y __builtin_prefetch
#else
N __builtin_prefetch
#endif
#if __has_builtin(__builtin_readcyclecounter)
Y __builtin_readcyclecounter
#else
N __builtin_readcyclecounter
#endif
#if __has_builtin(__builtin_rotateleft32)
Y __builtin_rotateleft32
#else
N __builtin_rotateleft32
#endif
#if __has_builtin(__builtin_round)
Y __builtin_round
#else
N __builtin_round
#endif
#if __has_builtin(__builtin_sadd_overflow)
Y __builtin_sadd_overflow
#else
N __builtin_sadd_overflow
#endif
#if __has_builtin(__builtin_shuffle)
Y __builtin_shuffle
#else
N __builtin_shuffle
#endif
#if __has_builtin(__builtin_shufflevector)
Y __builtin_shufflevector
#else
N __builtin_shufflevector
#endif
#if __has_builtin(__builtin_smul_overflow)
Y __builtin_smul_overflow
#else
N __builtin_smul_overflow
#endif
#if __has_builtin(__builtin_source_location)
Y __builtin_source_location
#else
N __builtin_source_location
#endif
#if __has_builtin(__builtin_spirv_ddx)
Y __builtin_spirv_ddx
#else
N __builtin_spirv_ddx
#endif
#if __has_builtin(__builtin_spirv_ddy)
Y __builtin_spirv_ddy
#else
N __builtin_spirv_ddy
#endif
#if __has_builtin(__builtin_spirv_fwidth)
Y __builtin_spirv_fwidth
#else
N __builtin_spirv_fwidth
#endif
#if __has_builtin(__builtin_spirv_length)
Y __builtin_spirv_length
#else
N __builtin_spirv_length
#endif
#if __has_builtin(__builtin_spirv_num_workgroups)
Y __builtin_spirv_num_workgroups
#else
N __builtin_spirv_num_workgroups
#endif
#if __has_builtin(__builtin_spirv_reflect)
Y __builtin_spirv_reflect
#else
N __builtin_spirv_reflect
#endif
#if __has_builtin(__builtin_spirv_refract)
Y __builtin_spirv_refract
#else
N __builtin_spirv_refract
#endif
#if __has_builtin(__builtin_spirv_smoothstep)
Y __builtin_spirv_smoothstep
#else
N __builtin_spirv_smoothstep
#endif
#if __has_builtin(__builtin_sponentry)
Y __builtin_sponentry
#else
N __builtin_sponentry
#endif
#if __has_builtin(__builtin_sprintf)
Y __builtin_sprintf
#else
N __builtin_sprintf
#endif
#if __has_builtin(__builtin_ssub_overflow)
Y __builtin_ssub_overflow
#else
N __builtin_ssub_overflow
#endif
#if __has_builtin(__builtin_stack_restore)
Y __builtin_stack_restore
#else
N __builtin_stack_restore
#endif
#if __has_builtin(__builtin_stack_save)
Y __builtin_stack_save
#else
N __builtin_stack_save
#endif
#if __has_builtin(__builtin_structured_binding_size)
Y __builtin_structured_binding_size
#else
N __builtin_structured_binding_size
#endif
#if __has_builtin(__builtin_sub_overflow)
Y __builtin_sub_overflow
#else
N __builtin_sub_overflow
#endif
#if __has_builtin(__builtin_thread_pointer)
Y __builtin_thread_pointer
#else
N __builtin_thread_pointer
#endif
#if __has_builtin(__builtin_toupper)
Y __builtin_toupper
#else
N __builtin_toupper
#endif
#if __has_builtin(__builtin_trap)
Y __builtin_trap
#else
N __builtin_trap
#endif
#if __has_builtin(__builtin_type_order)
Y __builtin_type_order
#else
N __builtin_type_order
#endif
#if __has_builtin(__builtin_types_compatible_p)
Y __builtin_types_compatible_p
#else
N __builtin_types_compatible_p
#endif
#if __has_builtin(__builtin_uadd_overflow)
Y __builtin_uadd_overflow
#else
N __builtin_uadd_overflow
#endif
#if __has_builtin(__builtin_umul_overflow)
Y __builtin_umul_overflow
#else
N __builtin_umul_overflow
#endif
#if __has_builtin(__builtin_unpredictable)
Y __builtin_unpredictable
#else
N __builtin_unpredictable
#endif
#if __has_builtin(__builtin_unreachable)
Y __builtin_unreachable
#else
N __builtin_unreachable
#endif
#if __has_builtin(__builtin_va_arg_pack)
Y __builtin_va_arg_pack
#else
N __builtin_va_arg_pack
#endif
#if __has_builtin(__builtin_wmemchr)
Y __builtin_wmemchr
#else
N __builtin_wmemchr
#endif
#if __has_builtin(__cpuid)
Y __cpuid
#else
N __cpuid
#endif
#if __has_builtin(__cpuidex)
Y __cpuidex
#else
N __cpuidex
#endif
#if __has_builtin(__debugbreak)
Y __debugbreak
#else
N __debugbreak
#endif
#if __has_builtin(__direct_bases)
Y __direct_bases
#else
N __direct_bases
#endif
#if __has_builtin(__fastfail)
Y __fastfail
#else
N __fastfail
#endif
#if __has_builtin(__faststorefence)
Y __faststorefence
#else
N __faststorefence
#endif
#if __has_builtin(__has_unique_object_representations)
Y __has_unique_object_representations
#else
N __has_unique_object_representations
#endif
#if __has_builtin(__inbyte)
Y __inbyte
#else
N __inbyte
#endif
#if __has_builtin(__inbytestring)
Y __inbytestring
#else
N __inbytestring
#endif
#if __has_builtin(__indword)
Y __indword
#else
N __indword
#endif
#if __has_builtin(__indwordstring)
Y __indwordstring
#else
N __indwordstring
#endif
#if __has_builtin(__int2c)
Y __int2c
#else
N __int2c
#endif
#if __has_builtin(__integer_pack)
Y __integer_pack
#else
N __integer_pack
#endif
#if __has_builtin(__inword)
Y __inword
#else
N __inword
#endif
#if __has_builtin(__inwordstring)
Y __inwordstring
#else
N __inwordstring
#endif
#if __has_builtin(__is_convertible)
Y __is_convertible
#else
N __is_convertible
#endif
#if __has_builtin(__is_layout_compatible)
Y __is_layout_compatible
#else
N __is_layout_compatible
#endif
#if __has_builtin(__is_pointer_interconvertible_base_of)
Y __is_pointer_interconvertible_base_of
#else
N __is_pointer_interconvertible_base_of
#endif
#if __has_builtin(__is_trivially_assignable)
Y __is_trivially_assignable
#else
N __is_trivially_assignable
#endif
#if __has_builtin(__is_trivially_constructible)
Y __is_trivially_constructible
#else
N __is_trivially_constructible
#endif
#if __has_builtin(__make_integer_seq)
Y __make_integer_seq
#else
N __make_integer_seq
#endif
#if __has_builtin(__movsb)
Y __movsb
#else
N __movsb
#endif
#if __has_builtin(__movsd)
Y __movsd
#else
N __movsd
#endif
#if __has_builtin(__movsq)
Y __movsq
#else
N __movsq
#endif
#if __has_builtin(__movsw)
Y __movsw
#else
N __movsw
#endif
#if __has_builtin(__outbyte)
Y __outbyte
#else
N __outbyte
#endif
#if __has_builtin(__outbytestring)
Y __outbytestring
#else
N __outbytestring
#endif
#if __has_builtin(__outdword)
Y __outdword
#else
N __outdword
#endif
#if __has_builtin(__outdwordstring)
Y __outdwordstring
#else
N __outdwordstring
#endif
#if __has_builtin(__outword)
Y __outword
#else
N __outword
#endif
#if __has_builtin(__outwordstring)
Y __outwordstring
#else
N __outwordstring
#endif
#if __has_builtin(__popcnt)
Y __popcnt
#else
N __popcnt
#endif
#if __has_builtin(__popcnt16)
Y __popcnt16
#else
N __popcnt16
#endif
#if __has_builtin(__popcnt64)
Y __popcnt64
#else
N __popcnt64
#endif
#if __has_builtin(__prefetch)
Y __prefetch
#else
N __prefetch
#endif
#if __has_builtin(__readcr0)
Y __readcr0
#else
N __readcr0
#endif
#if __has_builtin(__readcr2)
Y __readcr2
#else
N __readcr2
#endif
#if __has_builtin(__readcr3)
Y __readcr3
#else
N __readcr3
#endif
#if __has_builtin(__readcr4)
Y __readcr4
#else
N __readcr4
#endif
#if __has_builtin(__readcr8)
Y __readcr8
#else
N __readcr8
#endif
#if __has_builtin(__readfsbyte)
Y __readfsbyte
#else
N __readfsbyte
#endif
#if __has_builtin(__readfsdword)
Y __readfsdword
#else
N __readfsdword
#endif
#if __has_builtin(__readfsword)
Y __readfsword
#else
N __readfsword
#endif
#if __has_builtin(__readgsbyte)
Y __readgsbyte
#else
N __readgsbyte
#endif
#if __has_builtin(__readgsdword)
Y __readgsdword
#else
N __readgsdword
#endif
#if __has_builtin(__readgsqword)
Y __readgsqword
#else
N __readgsqword
#endif
#if __has_builtin(__readgsword)
Y __readgsword
#else
N __readgsword
#endif
#if __has_builtin(__readmsr)
Y __readmsr
#else
N __readmsr
#endif
#if __has_builtin(__reference_constructs_from_temporary)
Y __reference_constructs_from_temporary
#else
N __reference_constructs_from_temporary
#endif
#if __has_builtin(__reference_converts_from_temporary)
Y __reference_converts_from_temporary
#else
N __reference_converts_from_temporary
#endif
#if __has_builtin(__shiftleft128)
Y __shiftleft128
#else
N __shiftleft128
#endif
#if __has_builtin(__shiftright128)
Y __shiftright128
#else
N __shiftright128
#endif
#if __has_builtin(__stosb)
Y __stosb
#else
N __stosb
#endif
#if __has_builtin(__stosd)
Y __stosd
#else
N __stosd
#endif
#if __has_builtin(__stosq)
Y __stosq
#else
N __stosq
#endif
#if __has_builtin(__stosw)
Y __stosw
#else
N __stosw
#endif
#if __has_builtin(__type_pack_element)
Y __type_pack_element
#else
N __type_pack_element
#endif
#if __has_builtin(__writecr0)
Y __writecr0
#else
N __writecr0
#endif
#if __has_builtin(__writecr3)
Y __writecr3
#else
N __writecr3
#endif
#if __has_builtin(__writecr4)
Y __writecr4
#else
N __writecr4
#endif
#if __has_builtin(__writecr8)
Y __writecr8
#else
N __writecr8
#endif
#if __has_builtin(__writefsbyte)
Y __writefsbyte
#else
N __writefsbyte
#endif
#if __has_builtin(__writefsdword)
Y __writefsdword
#else
N __writefsdword
#endif
#if __has_builtin(__writefsword)
Y __writefsword
#else
N __writefsword
#endif
#if __has_builtin(__writegsbyte)
Y __writegsbyte
#else
N __writegsbyte
#endif
#if __has_builtin(__writegsdword)
Y __writegsdword
#else
N __writegsdword
#endif
#if __has_builtin(__writegsqword)
Y __writegsqword
#else
N __writegsqword
#endif
#if __has_builtin(__writegsword)
Y __writegsword
#else
N __writegsword
#endif
#if __has_builtin(__writemsr)
Y __writemsr
#else
N __writemsr
#endif
#if __has_builtin(__x)
Y __x
#else
N __x
#endif
#if __has_builtin(__yield)
Y __yield
#else
N __yield
#endif
