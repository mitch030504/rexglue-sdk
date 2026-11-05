// Generated from src/ui/shaders/nr_motion_prep_cs.glsl; do not edit by hand.
// Regenerate with
//   glslangValidator -V --target-env vulkan1.0 -S comp -o nr_motion_prep_cs.spv src/ui/shaders/nr_motion_prep_cs.glsl
// followed by tools/gen_nr_motion_prep_cs_header.py, which appends the words below.
#if 0
; SPIR-V
; Version: 1.0
; Generator: Khronos Glslang Reference Front End; 11
; Bound: 241
; Schema: 0
               OpCapability Shader
               OpCapability StorageImageExtendedFormats
          %1 = OpExtInstImport "GLSL.std.450"
               OpMemoryModel Logical GLSL450
               OpEntryPoint GLCompute %main "main" %gl_GlobalInvocationID
               OpExecutionMode %main LocalSize 8 8 1
               OpSource GLSL 450
               OpName %main "main"
               OpName %unpack_guide_vu2_ "unpack_guide(vu2;"
               OpName %word "word"
               OpName %px "px"
               OpName %gl_GlobalInvocationID "gl_GlobalInvocationID"
               OpName %PushConstants "PushConstants"
               OpMemberName %PushConstants 0 "out_extent"
               OpMemberName %PushConstants 1 "scene_extent"
               OpName %pc "pc"
               OpName %scene_pos "scene_pos"
               OpName %aligned "aligned"
               OpName %base "base"
               OpName %p0 "p0"
               OpName %p1 "p1"
               OpName %f "f"
               OpName %row0 "row0"
               OpName %row1 "row1"
               OpName %c00 "c00"
               OpName %GuideIn "GuideIn"
               OpMemberName %GuideIn 0 "pixels"
               OpName %_ ""
               OpName %param "param"
               OpName %c10 "c10"
               OpName %param_0 "param"
               OpName %c01 "c01"
               OpName %param_1 "param"
               OpName %c11 "c11"
               OpName %param_2 "param"
               OpName %m "m"
               OpName %motion "motion"
               OpName %motion_out "motion_out"
               OpName %cuv "cuv"
               OpName %colour_out "colour_out"
               OpName %colour_src "colour_src"
               OpDecorate %gl_GlobalInvocationID BuiltIn GlobalInvocationId
               OpDecorate %PushConstants Block
               OpMemberDecorate %PushConstants 0 Offset 0
               OpMemberDecorate %PushConstants 1 Offset 8
               OpDecorate %_runtimearr_v2uint ArrayStride 8
               OpDecorate %GuideIn BufferBlock
               OpMemberDecorate %GuideIn 0 NonWritable
               OpMemberDecorate %GuideIn 0 Offset 0
               OpDecorate %_ NonWritable
               OpDecorate %_ Binding 0
               OpDecorate %_ DescriptorSet 0
               OpDecorate %motion_out NonReadable
               OpDecorate %motion_out Binding 1
               OpDecorate %motion_out DescriptorSet 0
               OpDecorate %colour_out NonReadable
               OpDecorate %colour_out Binding 3
               OpDecorate %colour_out DescriptorSet 0
               OpDecorate %colour_src Binding 2
               OpDecorate %colour_src DescriptorSet 0
               OpDecorate %gl_WorkGroupSize BuiltIn WorkgroupSize
       %void = OpTypeVoid
          %3 = OpTypeFunction %void
       %uint = OpTypeInt 32 0
     %v2uint = OpTypeVector %uint 2
%_ptr_Function_v2uint = OpTypePointer Function %v2uint
      %float = OpTypeFloat 32
    %v4float = OpTypeVector %float 4
         %11 = OpTypeFunction %v4float %_ptr_Function_v2uint
     %uint_0 = OpConstant %uint 0
%_ptr_Function_uint = OpTypePointer Function %uint
    %v2float = OpTypeVector %float 2
     %uint_1 = OpConstant %uint 1
     %v3uint = OpTypeVector %uint 3
%_ptr_Input_v3uint = OpTypePointer Input %v3uint
%gl_GlobalInvocationID = OpVariable %_ptr_Input_v3uint Input
       %bool = OpTypeBool
%PushConstants = OpTypeStruct %v2uint %v2uint
%_ptr_PushConstant_PushConstants = OpTypePointer PushConstant %PushConstants
         %pc = OpVariable %_ptr_PushConstant_PushConstants PushConstant
        %int = OpTypeInt 32 1
      %int_0 = OpConstant %int 0
%_ptr_PushConstant_uint = OpTypePointer PushConstant %uint
%_ptr_Function_v2float = OpTypePointer Function %v2float
  %float_0_5 = OpConstant %float 0.5
      %int_1 = OpConstant %int 1
%_ptr_PushConstant_v2uint = OpTypePointer PushConstant %v2uint
      %v2int = OpTypeVector %int 2
%_ptr_Function_v2int = OpTypePointer Function %v2int
         %91 = OpConstantComposite %v2int %int_0 %int_0
    %float_0 = OpConstant %float 0
    %float_1 = OpConstant %float 1
%_ptr_Function_int = OpTypePointer Function %int
%_ptr_Function_v4float = OpTypePointer Function %v4float
%_runtimearr_v2uint = OpTypeRuntimeArray %v2uint
    %GuideIn = OpTypeStruct %_runtimearr_v2uint
%_ptr_Uniform_GuideIn = OpTypePointer Uniform %GuideIn
          %_ = OpVariable %_ptr_Uniform_GuideIn Uniform
%_ptr_Uniform_v2uint = OpTypePointer Uniform %v2uint
%_ptr_Function_float = OpTypePointer Function %float
     %uint_3 = OpConstant %uint 3
        %206 = OpConstantComposite %v2float %float_0 %float_0
        %207 = OpTypeImage %float 2D 0 0 0 2 Rg16f
%_ptr_UniformConstant_207 = OpTypePointer UniformConstant %207
 %motion_out = OpVariable %_ptr_UniformConstant_207 UniformConstant
        %226 = OpTypeImage %float 2D 0 0 0 2 Rgba8
%_ptr_UniformConstant_226 = OpTypePointer UniformConstant %226
 %colour_out = OpVariable %_ptr_UniformConstant_226 UniformConstant
        %232 = OpTypeImage %float 2D 0 0 0 1 Unknown
        %233 = OpTypeSampledImage %232
%_ptr_UniformConstant_233 = OpTypePointer UniformConstant %233
 %colour_src = OpVariable %_ptr_UniformConstant_233 UniformConstant
     %uint_8 = OpConstant %uint 8
%gl_WorkGroupSize = OpConstantComposite %v3uint %uint_8 %uint_8 %uint_1
       %main = OpFunction %void None %3
          %5 = OpLabel
         %px = OpVariable %_ptr_Function_v2uint Function
  %scene_pos = OpVariable %_ptr_Function_v2float Function
    %aligned = OpVariable %_ptr_Function_v2float Function
       %base = OpVariable %_ptr_Function_v2int Function
         %p0 = OpVariable %_ptr_Function_v2int Function
         %p1 = OpVariable %_ptr_Function_v2int Function
          %f = OpVariable %_ptr_Function_v2float Function
       %row0 = OpVariable %_ptr_Function_uint Function
       %row1 = OpVariable %_ptr_Function_uint Function
        %c00 = OpVariable %_ptr_Function_v4float Function
      %param = OpVariable %_ptr_Function_v2uint Function
        %c10 = OpVariable %_ptr_Function_v4float Function
    %param_0 = OpVariable %_ptr_Function_v2uint Function
        %c01 = OpVariable %_ptr_Function_v4float Function
    %param_1 = OpVariable %_ptr_Function_v2uint Function
        %c11 = OpVariable %_ptr_Function_v4float Function
    %param_2 = OpVariable %_ptr_Function_v2uint Function
          %m = OpVariable %_ptr_Function_v4float Function
     %motion = OpVariable %_ptr_Function_v2float Function
        %cuv = OpVariable %_ptr_Function_v2float Function
         %36 = OpLoad %v3uint %gl_GlobalInvocationID
         %37 = OpVectorShuffle %v2uint %36 %36 0 1
               OpStore %px %37
         %39 = OpAccessChain %_ptr_Function_uint %px %uint_0
         %40 = OpLoad %uint %39
         %47 = OpAccessChain %_ptr_PushConstant_uint %pc %int_0 %uint_0
         %48 = OpLoad %uint %47
         %49 = OpUGreaterThanEqual %bool %40 %48
         %50 = OpLogicalNot %bool %49
               OpSelectionMerge %52 None
               OpBranchConditional %50 %51 %52
         %51 = OpLabel
         %53 = OpAccessChain %_ptr_Function_uint %px %uint_1
         %54 = OpLoad %uint %53
         %55 = OpAccessChain %_ptr_PushConstant_uint %pc %int_0 %uint_1
         %56 = OpLoad %uint %55
         %57 = OpUGreaterThanEqual %bool %54 %56
               OpBranch %52
         %52 = OpLabel
         %58 = OpPhi %bool %49 %5 %57 %51
               OpSelectionMerge %60 None
               OpBranchConditional %58 %59 %60
         %59 = OpLabel
               OpReturn
         %60 = OpLabel
         %64 = OpLoad %v2uint %px
         %65 = OpConvertUToF %v2float %64
         %67 = OpCompositeConstruct %v2float %float_0_5 %float_0_5
         %68 = OpFAdd %v2float %65 %67
         %71 = OpAccessChain %_ptr_PushConstant_v2uint %pc %int_1
         %72 = OpLoad %v2uint %71
         %73 = OpConvertUToF %v2float %72
         %74 = OpFMul %v2float %68 %73
         %75 = OpAccessChain %_ptr_PushConstant_v2uint %pc %int_0
         %76 = OpLoad %v2uint %75
         %77 = OpConvertUToF %v2float %76
         %78 = OpFDiv %v2float %74 %77
               OpStore %scene_pos %78
         %80 = OpLoad %v2float %scene_pos
         %81 = OpCompositeConstruct %v2float %float_0_5 %float_0_5
         %82 = OpFSub %v2float %80 %81
               OpStore %aligned %82
         %86 = OpLoad %v2float %aligned
         %87 = OpExtInst %v2float %1 Floor %86
         %88 = OpConvertFToS %v2int %87
               OpStore %base %88
         %90 = OpLoad %v2int %base
         %92 = OpAccessChain %_ptr_PushConstant_v2uint %pc %int_1
         %93 = OpLoad %v2uint %92
         %94 = OpBitcast %v2int %93
         %95 = OpCompositeConstruct %v2int %int_1 %int_1
         %96 = OpISub %v2int %94 %95
         %97 = OpExtInst %v2int %1 SClamp %90 %91 %96
               OpStore %p0 %97
         %99 = OpLoad %v2int %p0
        %100 = OpCompositeConstruct %v2int %int_1 %int_1
        %101 = OpIAdd %v2int %99 %100
        %102 = OpAccessChain %_ptr_PushConstant_v2uint %pc %int_1
        %103 = OpLoad %v2uint %102
        %104 = OpBitcast %v2int %103
        %105 = OpCompositeConstruct %v2int %int_1 %int_1
        %106 = OpISub %v2int %104 %105
        %107 = OpExtInst %v2int %1 SMin %101 %106
               OpStore %p1 %107
        %109 = OpLoad %v2float %aligned
        %110 = OpLoad %v2int %base
        %111 = OpConvertSToF %v2float %110
        %112 = OpFSub %v2float %109 %111
        %115 = OpCompositeConstruct %v2float %float_0 %float_0
        %116 = OpCompositeConstruct %v2float %float_1 %float_1
        %117 = OpExtInst %v2float %1 FClamp %112 %115 %116
               OpStore %f %117
        %120 = OpAccessChain %_ptr_Function_int %p0 %uint_1
        %121 = OpLoad %int %120
        %122 = OpBitcast %uint %121
        %123 = OpAccessChain %_ptr_PushConstant_uint %pc %int_1 %uint_0
        %124 = OpLoad %uint %123
        %125 = OpIMul %uint %122 %124
               OpStore %row0 %125
        %127 = OpAccessChain %_ptr_Function_int %p1 %uint_1
        %128 = OpLoad %int %127
        %129 = OpBitcast %uint %128
        %130 = OpAccessChain %_ptr_PushConstant_uint %pc %int_1 %uint_0
        %131 = OpLoad %uint %130
        %132 = OpIMul %uint %129 %131
               OpStore %row1 %132
        %139 = OpLoad %uint %row0
        %140 = OpAccessChain %_ptr_Function_int %p0 %uint_0
        %141 = OpLoad %int %140
        %142 = OpBitcast %uint %141
        %143 = OpIAdd %uint %139 %142
        %146 = OpAccessChain %_ptr_Uniform_v2uint %_ %int_0 %143
        %147 = OpLoad %v2uint %146
               OpStore %param %147
        %148 = OpFunctionCall %v4float %unpack_guide_vu2_ %param
               OpStore %c00 %148
        %150 = OpLoad %uint %row0
        %151 = OpAccessChain %_ptr_Function_int %p1 %uint_0
        %152 = OpLoad %int %151
        %153 = OpBitcast %uint %152
        %154 = OpIAdd %uint %150 %153
        %156 = OpAccessChain %_ptr_Uniform_v2uint %_ %int_0 %154
        %157 = OpLoad %v2uint %156
               OpStore %param_0 %157
        %158 = OpFunctionCall %v4float %unpack_guide_vu2_ %param_0
               OpStore %c10 %158
        %160 = OpLoad %uint %row1
        %161 = OpAccessChain %_ptr_Function_int %p0 %uint_0
        %162 = OpLoad %int %161
        %163 = OpBitcast %uint %162
        %164 = OpIAdd %uint %160 %163
        %166 = OpAccessChain %_ptr_Uniform_v2uint %_ %int_0 %164
        %167 = OpLoad %v2uint %166
               OpStore %param_1 %167
        %168 = OpFunctionCall %v4float %unpack_guide_vu2_ %param_1
               OpStore %c01 %168
        %170 = OpLoad %uint %row1
        %171 = OpAccessChain %_ptr_Function_int %p1 %uint_0
        %172 = OpLoad %int %171
        %173 = OpBitcast %uint %172
        %174 = OpIAdd %uint %170 %173
        %176 = OpAccessChain %_ptr_Uniform_v2uint %_ %int_0 %174
        %177 = OpLoad %v2uint %176
               OpStore %param_2 %177
        %178 = OpFunctionCall %v4float %unpack_guide_vu2_ %param_2
               OpStore %c11 %178
        %180 = OpLoad %v4float %c00
        %181 = OpLoad %v4float %c10
        %183 = OpAccessChain %_ptr_Function_float %f %uint_0
        %184 = OpLoad %float %183
        %185 = OpCompositeConstruct %v4float %184 %184 %184 %184
        %186 = OpExtInst %v4float %1 FMix %180 %181 %185
        %187 = OpLoad %v4float %c01
        %188 = OpLoad %v4float %c11
        %189 = OpAccessChain %_ptr_Function_float %f %uint_0
        %190 = OpLoad %float %189
        %191 = OpCompositeConstruct %v4float %190 %190 %190 %190
        %192 = OpExtInst %v4float %1 FMix %187 %188 %191
        %193 = OpAccessChain %_ptr_Function_float %f %uint_1
        %194 = OpLoad %float %193
        %195 = OpCompositeConstruct %v4float %194 %194 %194 %194
        %196 = OpExtInst %v4float %1 FMix %186 %192 %195
               OpStore %m %196
        %198 = OpLoad %v4float %m
        %199 = OpVectorShuffle %v2float %198 %198 0 1
               OpStore %motion %199
        %201 = OpAccessChain %_ptr_Function_float %m %uint_3
        %202 = OpLoad %float %201
        %203 = OpFOrdLessThanEqual %bool %202 %float_0
               OpSelectionMerge %205 None
               OpBranchConditional %203 %204 %205
        %204 = OpLabel
               OpStore %motion %206
               OpBranch %205
        %205 = OpLabel
        %210 = OpLoad %207 %motion_out
        %211 = OpLoad %v2uint %px
        %212 = OpBitcast %v2int %211
        %213 = OpLoad %v2float %motion
        %214 = OpCompositeExtract %float %213 0
        %215 = OpCompositeExtract %float %213 1
        %216 = OpCompositeConstruct %v4float %214 %215 %float_0 %float_1
               OpImageWrite %210 %212 %216
        %218 = OpLoad %v2uint %px
        %219 = OpConvertUToF %v2float %218
        %220 = OpCompositeConstruct %v2float %float_0_5 %float_0_5
        %221 = OpFAdd %v2float %219 %220
        %222 = OpAccessChain %_ptr_PushConstant_v2uint %pc %int_0
        %223 = OpLoad %v2uint %222
        %224 = OpConvertUToF %v2float %223
        %225 = OpFDiv %v2float %221 %224
               OpStore %cuv %225
        %229 = OpLoad %226 %colour_out
        %230 = OpLoad %v2uint %px
        %231 = OpBitcast %v2int %230
        %236 = OpLoad %233 %colour_src
        %237 = OpLoad %v2float %cuv
        %238 = OpImageSampleExplicitLod %v4float %236 %237 Lod %float_0
               OpImageWrite %229 %231 %238
               OpReturn
               OpFunctionEnd
%unpack_guide_vu2_ = OpFunction %v4float None %11
       %word = OpFunctionParameter %_ptr_Function_v2uint
         %14 = OpLabel
         %17 = OpAccessChain %_ptr_Function_uint %word %uint_0
         %18 = OpLoad %uint %17
         %20 = OpExtInst %v2float %1 UnpackHalf2x16 %18
         %22 = OpAccessChain %_ptr_Function_uint %word %uint_1
         %23 = OpLoad %uint %22
         %24 = OpExtInst %v2float %1 UnpackHalf2x16 %23
         %25 = OpCompositeExtract %float %20 0
         %26 = OpCompositeExtract %float %20 1
         %27 = OpCompositeExtract %float %24 0
         %28 = OpCompositeExtract %float %24 1
         %29 = OpCompositeConstruct %v4float %25 %26 %27 %28
               OpReturnValue %29
               OpFunctionEnd
#endif

const uint32_t nr_motion_prep_cs[] = {
    0x07230203, 0x00010000, 0x0008000B, 0x000000F1, 0x00000000, 0x00020011, 0x00000001, 0x00020011,
    0x00000031, 0x0006000B, 0x00000001, 0x4C534C47, 0x6474732E, 0x3035342E, 0x00000000, 0x0003000E,
    0x00000000, 0x00000001, 0x0006000F, 0x00000005, 0x00000004, 0x6E69616D, 0x00000000, 0x00000023,
    0x00060010, 0x00000004, 0x00000011, 0x00000008, 0x00000008, 0x00000001, 0x00030003, 0x00000002,
    0x000001C2, 0x00040005, 0x00000004, 0x6E69616D, 0x00000000, 0x00070005, 0x0000000D, 0x61706E75,
    0x675F6B63, 0x65646975, 0x32757628, 0x0000003B, 0x00040005, 0x0000000C, 0x64726F77, 0x00000000,
    0x00030005, 0x00000020, 0x00007870, 0x00080005, 0x00000023, 0x475F6C67, 0x61626F6C, 0x766E496C,
    0x7461636F, 0x496E6F69, 0x00000044, 0x00060005, 0x00000029, 0x68737550, 0x736E6F43, 0x746E6174,
    0x00000073, 0x00060006, 0x00000029, 0x00000000, 0x5F74756F, 0x65747865, 0x0000746E, 0x00070006,
    0x00000029, 0x00000001, 0x6E656373, 0x78655F65, 0x746E6574, 0x00000000, 0x00030005, 0x0000002B,
    0x00006370, 0x00050005, 0x0000003F, 0x6E656373, 0x6F705F65, 0x00000073, 0x00040005, 0x0000004F,
    0x67696C61, 0x0064656E, 0x00040005, 0x00000055, 0x65736162, 0x00000000, 0x00030005, 0x00000059,
    0x00003070, 0x00030005, 0x00000062, 0x00003170, 0x00030005, 0x0000006C, 0x00000066, 0x00040005,
    0x00000076, 0x30776F72, 0x00000000, 0x00040005, 0x0000007E, 0x31776F72, 0x00000000, 0x00030005,
    0x00000086, 0x00303063, 0x00040005, 0x00000088, 0x64697547, 0x006E4965, 0x00050006, 0x00000088,
    0x00000000, 0x65786970, 0x0000736C, 0x00030005, 0x0000008A, 0x00000000, 0x00040005, 0x00000090,
    0x61726170, 0x0000006D, 0x00030005, 0x00000095, 0x00303163, 0x00040005, 0x0000009B, 0x61726170,
    0x0000006D, 0x00030005, 0x0000009F, 0x00313063, 0x00040005, 0x000000A5, 0x61726170, 0x0000006D,
    0x00030005, 0x000000A9, 0x00313163, 0x00040005, 0x000000AF, 0x61726170, 0x0000006D, 0x00030005,
    0x000000B3, 0x0000006D, 0x00040005, 0x000000C5, 0x69746F6D, 0x00006E6F, 0x00050005, 0x000000D1,
    0x69746F6D, 0x6F5F6E6F, 0x00007475, 0x00030005, 0x000000D9, 0x00767563, 0x00050005, 0x000000E4,
    0x6F6C6F63, 0x6F5F7275, 0x00007475, 0x00050005, 0x000000EB, 0x6F6C6F63, 0x735F7275, 0x00006372,
    0x00040047, 0x00000023, 0x0000000B, 0x0000001C, 0x00030047, 0x00000029, 0x00000002, 0x00050048,
    0x00000029, 0x00000000, 0x00000023, 0x00000000, 0x00050048, 0x00000029, 0x00000001, 0x00000023,
    0x00000008, 0x00040047, 0x00000087, 0x00000006, 0x00000008, 0x00030047, 0x00000088, 0x00000003,
    0x00040048, 0x00000088, 0x00000000, 0x00000018, 0x00050048, 0x00000088, 0x00000000, 0x00000023,
    0x00000000, 0x00030047, 0x0000008A, 0x00000018, 0x00040047, 0x0000008A, 0x00000021, 0x00000000,
    0x00040047, 0x0000008A, 0x00000022, 0x00000000, 0x00030047, 0x000000D1, 0x00000019, 0x00040047,
    0x000000D1, 0x00000021, 0x00000001, 0x00040047, 0x000000D1, 0x00000022, 0x00000000, 0x00030047,
    0x000000E4, 0x00000019, 0x00040047, 0x000000E4, 0x00000021, 0x00000003, 0x00040047, 0x000000E4,
    0x00000022, 0x00000000, 0x00040047, 0x000000EB, 0x00000021, 0x00000002, 0x00040047, 0x000000EB,
    0x00000022, 0x00000000, 0x00040047, 0x000000F0, 0x0000000B, 0x00000019, 0x00020013, 0x00000002,
    0x00030021, 0x00000003, 0x00000002, 0x00040015, 0x00000006, 0x00000020, 0x00000000, 0x00040017,
    0x00000007, 0x00000006, 0x00000002, 0x00040020, 0x00000008, 0x00000007, 0x00000007, 0x00030016,
    0x00000009, 0x00000020, 0x00040017, 0x0000000A, 0x00000009, 0x00000004, 0x00040021, 0x0000000B,
    0x0000000A, 0x00000008, 0x0004002B, 0x00000006, 0x0000000F, 0x00000000, 0x00040020, 0x00000010,
    0x00000007, 0x00000006, 0x00040017, 0x00000013, 0x00000009, 0x00000002, 0x0004002B, 0x00000006,
    0x00000015, 0x00000001, 0x00040017, 0x00000021, 0x00000006, 0x00000003, 0x00040020, 0x00000022,
    0x00000001, 0x00000021, 0x0004003B, 0x00000022, 0x00000023, 0x00000001, 0x00020014, 0x00000026,
    0x0004001E, 0x00000029, 0x00000007, 0x00000007, 0x00040020, 0x0000002A, 0x00000009, 0x00000029,
    0x0004003B, 0x0000002A, 0x0000002B, 0x00000009, 0x00040015, 0x0000002C, 0x00000020, 0x00000001,
    0x0004002B, 0x0000002C, 0x0000002D, 0x00000000, 0x00040020, 0x0000002E, 0x00000009, 0x00000006,
    0x00040020, 0x0000003E, 0x00000007, 0x00000013, 0x0004002B, 0x00000009, 0x00000042, 0x3F000000,
    0x0004002B, 0x0000002C, 0x00000045, 0x00000001, 0x00040020, 0x00000046, 0x00000009, 0x00000007,
    0x00040017, 0x00000053, 0x0000002C, 0x00000002, 0x00040020, 0x00000054, 0x00000007, 0x00000053,
    0x0005002C, 0x00000053, 0x0000005B, 0x0000002D, 0x0000002D, 0x0004002B, 0x00000009, 0x00000071,
    0x00000000, 0x0004002B, 0x00000009, 0x00000072, 0x3F800000, 0x00040020, 0x00000077, 0x00000007,
    0x0000002C, 0x00040020, 0x00000085, 0x00000007, 0x0000000A, 0x0003001D, 0x00000087, 0x00000007,
    0x0003001E, 0x00000088, 0x00000087, 0x00040020, 0x00000089, 0x00000002, 0x00000088, 0x0004003B,
    0x00000089, 0x0000008A, 0x00000002, 0x00040020, 0x00000091, 0x00000002, 0x00000007, 0x00040020,
    0x000000B6, 0x00000007, 0x00000009, 0x0004002B, 0x00000006, 0x000000C8, 0x00000003, 0x0005002C,
    0x00000013, 0x000000CE, 0x00000071, 0x00000071, 0x00090019, 0x000000CF, 0x00000009, 0x00000001,
    0x00000000, 0x00000000, 0x00000000, 0x00000002, 0x00000007, 0x00040020, 0x000000D0, 0x00000000,
    0x000000CF, 0x0004003B, 0x000000D0, 0x000000D1, 0x00000000, 0x00090019, 0x000000E2, 0x00000009,
    0x00000001, 0x00000000, 0x00000000, 0x00000000, 0x00000002, 0x00000004, 0x00040020, 0x000000E3,
    0x00000000, 0x000000E2, 0x0004003B, 0x000000E3, 0x000000E4, 0x00000000, 0x00090019, 0x000000E8,
    0x00000009, 0x00000001, 0x00000000, 0x00000000, 0x00000000, 0x00000001, 0x00000000, 0x0003001B,
    0x000000E9, 0x000000E8, 0x00040020, 0x000000EA, 0x00000000, 0x000000E9, 0x0004003B, 0x000000EA,
    0x000000EB, 0x00000000, 0x0004002B, 0x00000006, 0x000000EF, 0x00000008, 0x0006002C, 0x00000021,
    0x000000F0, 0x000000EF, 0x000000EF, 0x00000015, 0x00050036, 0x00000002, 0x00000004, 0x00000000,
    0x00000003, 0x000200F8, 0x00000005, 0x0004003B, 0x00000008, 0x00000020, 0x00000007, 0x0004003B,
    0x0000003E, 0x0000003F, 0x00000007, 0x0004003B, 0x0000003E, 0x0000004F, 0x00000007, 0x0004003B,
    0x00000054, 0x00000055, 0x00000007, 0x0004003B, 0x00000054, 0x00000059, 0x00000007, 0x0004003B,
    0x00000054, 0x00000062, 0x00000007, 0x0004003B, 0x0000003E, 0x0000006C, 0x00000007, 0x0004003B,
    0x00000010, 0x00000076, 0x00000007, 0x0004003B, 0x00000010, 0x0000007E, 0x00000007, 0x0004003B,
    0x00000085, 0x00000086, 0x00000007, 0x0004003B, 0x00000008, 0x00000090, 0x00000007, 0x0004003B,
    0x00000085, 0x00000095, 0x00000007, 0x0004003B, 0x00000008, 0x0000009B, 0x00000007, 0x0004003B,
    0x00000085, 0x0000009F, 0x00000007, 0x0004003B, 0x00000008, 0x000000A5, 0x00000007, 0x0004003B,
    0x00000085, 0x000000A9, 0x00000007, 0x0004003B, 0x00000008, 0x000000AF, 0x00000007, 0x0004003B,
    0x00000085, 0x000000B3, 0x00000007, 0x0004003B, 0x0000003E, 0x000000C5, 0x00000007, 0x0004003B,
    0x0000003E, 0x000000D9, 0x00000007, 0x0004003D, 0x00000021, 0x00000024, 0x00000023, 0x0007004F,
    0x00000007, 0x00000025, 0x00000024, 0x00000024, 0x00000000, 0x00000001, 0x0003003E, 0x00000020,
    0x00000025, 0x00050041, 0x00000010, 0x00000027, 0x00000020, 0x0000000F, 0x0004003D, 0x00000006,
    0x00000028, 0x00000027, 0x00060041, 0x0000002E, 0x0000002F, 0x0000002B, 0x0000002D, 0x0000000F,
    0x0004003D, 0x00000006, 0x00000030, 0x0000002F, 0x000500AE, 0x00000026, 0x00000031, 0x00000028,
    0x00000030, 0x000400A8, 0x00000026, 0x00000032, 0x00000031, 0x000300F7, 0x00000034, 0x00000000,
    0x000400FA, 0x00000032, 0x00000033, 0x00000034, 0x000200F8, 0x00000033, 0x00050041, 0x00000010,
    0x00000035, 0x00000020, 0x00000015, 0x0004003D, 0x00000006, 0x00000036, 0x00000035, 0x00060041,
    0x0000002E, 0x00000037, 0x0000002B, 0x0000002D, 0x00000015, 0x0004003D, 0x00000006, 0x00000038,
    0x00000037, 0x000500AE, 0x00000026, 0x00000039, 0x00000036, 0x00000038, 0x000200F9, 0x00000034,
    0x000200F8, 0x00000034, 0x000700F5, 0x00000026, 0x0000003A, 0x00000031, 0x00000005, 0x00000039,
    0x00000033, 0x000300F7, 0x0000003C, 0x00000000, 0x000400FA, 0x0000003A, 0x0000003B, 0x0000003C,
    0x000200F8, 0x0000003B, 0x000100FD, 0x000200F8, 0x0000003C, 0x0004003D, 0x00000007, 0x00000040,
    0x00000020, 0x00040070, 0x00000013, 0x00000041, 0x00000040, 0x00050050, 0x00000013, 0x00000043,
    0x00000042, 0x00000042, 0x00050081, 0x00000013, 0x00000044, 0x00000041, 0x00000043, 0x00050041,
    0x00000046, 0x00000047, 0x0000002B, 0x00000045, 0x0004003D, 0x00000007, 0x00000048, 0x00000047,
    0x00040070, 0x00000013, 0x00000049, 0x00000048, 0x00050085, 0x00000013, 0x0000004A, 0x00000044,
    0x00000049, 0x00050041, 0x00000046, 0x0000004B, 0x0000002B, 0x0000002D, 0x0004003D, 0x00000007,
    0x0000004C, 0x0000004B, 0x00040070, 0x00000013, 0x0000004D, 0x0000004C, 0x00050088, 0x00000013,
    0x0000004E, 0x0000004A, 0x0000004D, 0x0003003E, 0x0000003F, 0x0000004E, 0x0004003D, 0x00000013,
    0x00000050, 0x0000003F, 0x00050050, 0x00000013, 0x00000051, 0x00000042, 0x00000042, 0x00050083,
    0x00000013, 0x00000052, 0x00000050, 0x00000051, 0x0003003E, 0x0000004F, 0x00000052, 0x0004003D,
    0x00000013, 0x00000056, 0x0000004F, 0x0006000C, 0x00000013, 0x00000057, 0x00000001, 0x00000008,
    0x00000056, 0x0004006E, 0x00000053, 0x00000058, 0x00000057, 0x0003003E, 0x00000055, 0x00000058,
    0x0004003D, 0x00000053, 0x0000005A, 0x00000055, 0x00050041, 0x00000046, 0x0000005C, 0x0000002B,
    0x00000045, 0x0004003D, 0x00000007, 0x0000005D, 0x0000005C, 0x0004007C, 0x00000053, 0x0000005E,
    0x0000005D, 0x00050050, 0x00000053, 0x0000005F, 0x00000045, 0x00000045, 0x00050082, 0x00000053,
    0x00000060, 0x0000005E, 0x0000005F, 0x0008000C, 0x00000053, 0x00000061, 0x00000001, 0x0000002D,
    0x0000005A, 0x0000005B, 0x00000060, 0x0003003E, 0x00000059, 0x00000061, 0x0004003D, 0x00000053,
    0x00000063, 0x00000059, 0x00050050, 0x00000053, 0x00000064, 0x00000045, 0x00000045, 0x00050080,
    0x00000053, 0x00000065, 0x00000063, 0x00000064, 0x00050041, 0x00000046, 0x00000066, 0x0000002B,
    0x00000045, 0x0004003D, 0x00000007, 0x00000067, 0x00000066, 0x0004007C, 0x00000053, 0x00000068,
    0x00000067, 0x00050050, 0x00000053, 0x00000069, 0x00000045, 0x00000045, 0x00050082, 0x00000053,
    0x0000006A, 0x00000068, 0x00000069, 0x0007000C, 0x00000053, 0x0000006B, 0x00000001, 0x00000027,
    0x00000065, 0x0000006A, 0x0003003E, 0x00000062, 0x0000006B, 0x0004003D, 0x00000013, 0x0000006D,
    0x0000004F, 0x0004003D, 0x00000053, 0x0000006E, 0x00000055, 0x0004006F, 0x00000013, 0x0000006F,
    0x0000006E, 0x00050083, 0x00000013, 0x00000070, 0x0000006D, 0x0000006F, 0x00050050, 0x00000013,
    0x00000073, 0x00000071, 0x00000071, 0x00050050, 0x00000013, 0x00000074, 0x00000072, 0x00000072,
    0x0008000C, 0x00000013, 0x00000075, 0x00000001, 0x0000002B, 0x00000070, 0x00000073, 0x00000074,
    0x0003003E, 0x0000006C, 0x00000075, 0x00050041, 0x00000077, 0x00000078, 0x00000059, 0x00000015,
    0x0004003D, 0x0000002C, 0x00000079, 0x00000078, 0x0004007C, 0x00000006, 0x0000007A, 0x00000079,
    0x00060041, 0x0000002E, 0x0000007B, 0x0000002B, 0x00000045, 0x0000000F, 0x0004003D, 0x00000006,
    0x0000007C, 0x0000007B, 0x00050084, 0x00000006, 0x0000007D, 0x0000007A, 0x0000007C, 0x0003003E,
    0x00000076, 0x0000007D, 0x00050041, 0x00000077, 0x0000007F, 0x00000062, 0x00000015, 0x0004003D,
    0x0000002C, 0x00000080, 0x0000007F, 0x0004007C, 0x00000006, 0x00000081, 0x00000080, 0x00060041,
    0x0000002E, 0x00000082, 0x0000002B, 0x00000045, 0x0000000F, 0x0004003D, 0x00000006, 0x00000083,
    0x00000082, 0x00050084, 0x00000006, 0x00000084, 0x00000081, 0x00000083, 0x0003003E, 0x0000007E,
    0x00000084, 0x0004003D, 0x00000006, 0x0000008B, 0x00000076, 0x00050041, 0x00000077, 0x0000008C,
    0x00000059, 0x0000000F, 0x0004003D, 0x0000002C, 0x0000008D, 0x0000008C, 0x0004007C, 0x00000006,
    0x0000008E, 0x0000008D, 0x00050080, 0x00000006, 0x0000008F, 0x0000008B, 0x0000008E, 0x00060041,
    0x00000091, 0x00000092, 0x0000008A, 0x0000002D, 0x0000008F, 0x0004003D, 0x00000007, 0x00000093,
    0x00000092, 0x0003003E, 0x00000090, 0x00000093, 0x00050039, 0x0000000A, 0x00000094, 0x0000000D,
    0x00000090, 0x0003003E, 0x00000086, 0x00000094, 0x0004003D, 0x00000006, 0x00000096, 0x00000076,
    0x00050041, 0x00000077, 0x00000097, 0x00000062, 0x0000000F, 0x0004003D, 0x0000002C, 0x00000098,
    0x00000097, 0x0004007C, 0x00000006, 0x00000099, 0x00000098, 0x00050080, 0x00000006, 0x0000009A,
    0x00000096, 0x00000099, 0x00060041, 0x00000091, 0x0000009C, 0x0000008A, 0x0000002D, 0x0000009A,
    0x0004003D, 0x00000007, 0x0000009D, 0x0000009C, 0x0003003E, 0x0000009B, 0x0000009D, 0x00050039,
    0x0000000A, 0x0000009E, 0x0000000D, 0x0000009B, 0x0003003E, 0x00000095, 0x0000009E, 0x0004003D,
    0x00000006, 0x000000A0, 0x0000007E, 0x00050041, 0x00000077, 0x000000A1, 0x00000059, 0x0000000F,
    0x0004003D, 0x0000002C, 0x000000A2, 0x000000A1, 0x0004007C, 0x00000006, 0x000000A3, 0x000000A2,
    0x00050080, 0x00000006, 0x000000A4, 0x000000A0, 0x000000A3, 0x00060041, 0x00000091, 0x000000A6,
    0x0000008A, 0x0000002D, 0x000000A4, 0x0004003D, 0x00000007, 0x000000A7, 0x000000A6, 0x0003003E,
    0x000000A5, 0x000000A7, 0x00050039, 0x0000000A, 0x000000A8, 0x0000000D, 0x000000A5, 0x0003003E,
    0x0000009F, 0x000000A8, 0x0004003D, 0x00000006, 0x000000AA, 0x0000007E, 0x00050041, 0x00000077,
    0x000000AB, 0x00000062, 0x0000000F, 0x0004003D, 0x0000002C, 0x000000AC, 0x000000AB, 0x0004007C,
    0x00000006, 0x000000AD, 0x000000AC, 0x00050080, 0x00000006, 0x000000AE, 0x000000AA, 0x000000AD,
    0x00060041, 0x00000091, 0x000000B0, 0x0000008A, 0x0000002D, 0x000000AE, 0x0004003D, 0x00000007,
    0x000000B1, 0x000000B0, 0x0003003E, 0x000000AF, 0x000000B1, 0x00050039, 0x0000000A, 0x000000B2,
    0x0000000D, 0x000000AF, 0x0003003E, 0x000000A9, 0x000000B2, 0x0004003D, 0x0000000A, 0x000000B4,
    0x00000086, 0x0004003D, 0x0000000A, 0x000000B5, 0x00000095, 0x00050041, 0x000000B6, 0x000000B7,
    0x0000006C, 0x0000000F, 0x0004003D, 0x00000009, 0x000000B8, 0x000000B7, 0x00070050, 0x0000000A,
    0x000000B9, 0x000000B8, 0x000000B8, 0x000000B8, 0x000000B8, 0x0008000C, 0x0000000A, 0x000000BA,
    0x00000001, 0x0000002E, 0x000000B4, 0x000000B5, 0x000000B9, 0x0004003D, 0x0000000A, 0x000000BB,
    0x0000009F, 0x0004003D, 0x0000000A, 0x000000BC, 0x000000A9, 0x00050041, 0x000000B6, 0x000000BD,
    0x0000006C, 0x0000000F, 0x0004003D, 0x00000009, 0x000000BE, 0x000000BD, 0x00070050, 0x0000000A,
    0x000000BF, 0x000000BE, 0x000000BE, 0x000000BE, 0x000000BE, 0x0008000C, 0x0000000A, 0x000000C0,
    0x00000001, 0x0000002E, 0x000000BB, 0x000000BC, 0x000000BF, 0x00050041, 0x000000B6, 0x000000C1,
    0x0000006C, 0x00000015, 0x0004003D, 0x00000009, 0x000000C2, 0x000000C1, 0x00070050, 0x0000000A,
    0x000000C3, 0x000000C2, 0x000000C2, 0x000000C2, 0x000000C2, 0x0008000C, 0x0000000A, 0x000000C4,
    0x00000001, 0x0000002E, 0x000000BA, 0x000000C0, 0x000000C3, 0x0003003E, 0x000000B3, 0x000000C4,
    0x0004003D, 0x0000000A, 0x000000C6, 0x000000B3, 0x0007004F, 0x00000013, 0x000000C7, 0x000000C6,
    0x000000C6, 0x00000000, 0x00000001, 0x0003003E, 0x000000C5, 0x000000C7, 0x00050041, 0x000000B6,
    0x000000C9, 0x000000B3, 0x000000C8, 0x0004003D, 0x00000009, 0x000000CA, 0x000000C9, 0x000500BC,
    0x00000026, 0x000000CB, 0x000000CA, 0x00000071, 0x000300F7, 0x000000CD, 0x00000000, 0x000400FA,
    0x000000CB, 0x000000CC, 0x000000CD, 0x000200F8, 0x000000CC, 0x0003003E, 0x000000C5, 0x000000CE,
    0x000200F9, 0x000000CD, 0x000200F8, 0x000000CD, 0x0004003D, 0x000000CF, 0x000000D2, 0x000000D1,
    0x0004003D, 0x00000007, 0x000000D3, 0x00000020, 0x0004007C, 0x00000053, 0x000000D4, 0x000000D3,
    0x0004003D, 0x00000013, 0x000000D5, 0x000000C5, 0x00050051, 0x00000009, 0x000000D6, 0x000000D5,
    0x00000000, 0x00050051, 0x00000009, 0x000000D7, 0x000000D5, 0x00000001, 0x00070050, 0x0000000A,
    0x000000D8, 0x000000D6, 0x000000D7, 0x00000071, 0x00000072, 0x00040063, 0x000000D2, 0x000000D4,
    0x000000D8, 0x0004003D, 0x00000007, 0x000000DA, 0x00000020, 0x00040070, 0x00000013, 0x000000DB,
    0x000000DA, 0x00050050, 0x00000013, 0x000000DC, 0x00000042, 0x00000042, 0x00050081, 0x00000013,
    0x000000DD, 0x000000DB, 0x000000DC, 0x00050041, 0x00000046, 0x000000DE, 0x0000002B, 0x0000002D,
    0x0004003D, 0x00000007, 0x000000DF, 0x000000DE, 0x00040070, 0x00000013, 0x000000E0, 0x000000DF,
    0x00050088, 0x00000013, 0x000000E1, 0x000000DD, 0x000000E0, 0x0003003E, 0x000000D9, 0x000000E1,
    0x0004003D, 0x000000E2, 0x000000E5, 0x000000E4, 0x0004003D, 0x00000007, 0x000000E6, 0x00000020,
    0x0004007C, 0x00000053, 0x000000E7, 0x000000E6, 0x0004003D, 0x000000E9, 0x000000EC, 0x000000EB,
    0x0004003D, 0x00000013, 0x000000ED, 0x000000D9, 0x00070058, 0x0000000A, 0x000000EE, 0x000000EC,
    0x000000ED, 0x00000002, 0x00000071, 0x00040063, 0x000000E5, 0x000000E7, 0x000000EE, 0x000100FD,
    0x00010038, 0x00050036, 0x0000000A, 0x0000000D, 0x00000000, 0x0000000B, 0x00030037, 0x00000008,
    0x0000000C, 0x000200F8, 0x0000000E, 0x00050041, 0x00000010, 0x00000011, 0x0000000C, 0x0000000F,
    0x0004003D, 0x00000006, 0x00000012, 0x00000011, 0x0006000C, 0x00000013, 0x00000014, 0x00000001,
    0x0000003E, 0x00000012, 0x00050041, 0x00000010, 0x00000016, 0x0000000C, 0x00000015, 0x0004003D,
    0x00000006, 0x00000017, 0x00000016, 0x0006000C, 0x00000013, 0x00000018, 0x00000001, 0x0000003E,
    0x00000017, 0x00050051, 0x00000009, 0x00000019, 0x00000014, 0x00000000, 0x00050051, 0x00000009,
    0x0000001A, 0x00000014, 0x00000001, 0x00050051, 0x00000009, 0x0000001B, 0x00000018, 0x00000000,
    0x00050051, 0x00000009, 0x0000001C, 0x00000018, 0x00000001, 0x00070050, 0x0000000A, 0x0000001D,
    0x00000019, 0x0000001A, 0x0000001B, 0x0000001C, 0x000200FE, 0x0000001D, 0x00010038,
};
