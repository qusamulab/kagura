; RUN: %opt -load-pass-plugin=%kagura_plugin -passes='kagura-dwarf-control,verify' -kagura-dwarf=obfuscate -S %s | %FileCheck %s --check-prefix=OBF
; RUN: %opt -load-pass-plugin=%kagura_plugin -passes='kagura-dwarf-control,verify' -kagura-dwarf=keep  -S %s | %FileCheck %s --check-prefix=KEEP
; RUN: %opt -load-pass-plugin=%kagura_plugin -passes='kagura-dwarf-control,verify' -kagura-dwarf=strip -S %s | %FileCheck %s --check-prefix=STRIP

; OBF: !DIFile(filename: "<kagura>"
; OBF: !DILocation(line: {{[0-9]+}}, scope:
; KEEP:  !DILocation
; STRIP-NOT: !DILocation

; Minimal IR with debug info attached to a function.
; The function has a single add instruction annotated with a DILocation.

define i32 @test_dwarf(i32 %a) !dbg !3 {
entry:
  %result = add i32 %a, 42, !dbg !8
  ret i32 %result, !dbg !8
}

!llvm.dbg.cu = !{!0}
!llvm.module.flags = !{!6, !7}

!0 = distinct !DICompileUnit(language: DW_LANG_C99, file: !1, producer: "clang", isOptimized: false, runtimeVersion: 0, emissionKind: FullDebug, enums: !2)
!1 = !DIFile(filename: "test.c", directory: "/tmp")
!2 = !{}
!3 = distinct !DISubprogram(name: "test_dwarf", linkageName: "test_dwarf", scope: !1, file: !1, line: 1, type: !4, scopeLine: 1, spFlags: DISPFlagDefinition, unit: !0)
!4 = !DISubroutineType(types: !5)
!5 = !{null}
!6 = !{i32 2, !"Dwarf Version", i32 4}
!7 = !{i32 2, !"Debug Info Version", i32 3}
!8 = !DILocation(line: 2, column: 10, scope: !3)
