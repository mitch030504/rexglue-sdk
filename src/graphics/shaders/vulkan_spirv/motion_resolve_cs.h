// Generated from src/graphics/shaders/motion_resolve_cs.glsl; do not edit by hand.
// Regenerate with
//   glslangValidator -V --target-env vulkan1.0 -S comp -o motion_resolve_cs.spv src/graphics/shaders/motion_resolve_cs.glsl
// followed by tools/gen_motion_resolve_cs_header.py, which appends the words below.
#if 0
; SPIR-V
; Version: 1.0
; Generator: Khronos Glslang Reference Front End; 11
; Bound: 72
; Schema: 0
               OpCapability Shader
          %1 = OpExtInstImport "GLSL.std.450"
               OpMemoryModel Logical GLSL450
               OpEntryPoint GLCompute %main "main" %gl_GlobalInvocationID
               OpExecutionMode %main LocalSize 8 8 1
               OpSource GLSL 450
               OpName %main "main"
               OpName %pixel "pixel"
               OpName %gl_GlobalInvocationID "gl_GlobalInvocationID"
               OpName %motion "motion"
               OpName %motion_image "motion_image"
               OpName %MotionOut "MotionOut"
               OpMemberName %MotionOut 0 "pixels"
               OpName %_ ""
               OpDecorate %gl_GlobalInvocationID BuiltIn GlobalInvocationId
               OpDecorate %motion_image Binding 0
               OpDecorate %motion_image DescriptorSet 0
               OpDecorate %_runtimearr_v2uint ArrayStride 8
               OpDecorate %MotionOut BufferBlock
               OpMemberDecorate %MotionOut 0 NonReadable
               OpMemberDecorate %MotionOut 0 Offset 0
               OpDecorate %_ NonReadable
               OpDecorate %_ Binding 1
               OpDecorate %_ DescriptorSet 0
               OpDecorate %gl_WorkGroupSize BuiltIn WorkgroupSize
       %void = OpTypeVoid
          %3 = OpTypeFunction %void
       %uint = OpTypeInt 32 0
     %v2uint = OpTypeVector %uint 2
%_ptr_Function_v2uint = OpTypePointer Function %v2uint
     %v3uint = OpTypeVector %uint 3
%_ptr_Input_v3uint = OpTypePointer Input %v3uint
%gl_GlobalInvocationID = OpVariable %_ptr_Input_v3uint Input
       %bool = OpTypeBool
     %uint_0 = OpConstant %uint 0
%_ptr_Function_uint = OpTypePointer Function %uint
  %uint_1000 = OpConstant %uint 1000
     %uint_1 = OpConstant %uint 1
   %uint_600 = OpConstant %uint 600
      %float = OpTypeFloat 32
    %v4float = OpTypeVector %float 4
%_ptr_Function_v4float = OpTypePointer Function %v4float
         %38 = OpTypeImage %float 2D 0 0 1 1 Unknown
         %39 = OpTypeSampledImage %38
%_ptr_UniformConstant_39 = OpTypePointer UniformConstant %39
%motion_image = OpVariable %_ptr_UniformConstant_39 UniformConstant
        %int = OpTypeInt 32 1
      %v2int = OpTypeVector %int 2
      %int_0 = OpConstant %int 0
%_runtimearr_v2uint = OpTypeRuntimeArray %v2uint
  %MotionOut = OpTypeStruct %_runtimearr_v2uint
%_ptr_Uniform_MotionOut = OpTypePointer Uniform %MotionOut
          %_ = OpVariable %_ptr_Uniform_MotionOut Uniform
    %v2float = OpTypeVector %float 2
%_ptr_Uniform_v2uint = OpTypePointer Uniform %v2uint
     %uint_8 = OpConstant %uint 8
%gl_WorkGroupSize = OpConstantComposite %v3uint %uint_8 %uint_8 %uint_1
       %main = OpFunction %void None %3
          %5 = OpLabel
      %pixel = OpVariable %_ptr_Function_v2uint Function
     %motion = OpVariable %_ptr_Function_v4float Function
         %13 = OpLoad %v3uint %gl_GlobalInvocationID
         %14 = OpVectorShuffle %v2uint %13 %13 0 1
               OpStore %pixel %14
         %18 = OpAccessChain %_ptr_Function_uint %pixel %uint_0
         %19 = OpLoad %uint %18
         %21 = OpUGreaterThanEqual %bool %19 %uint_1000
         %22 = OpLogicalNot %bool %21
               OpSelectionMerge %24 None
               OpBranchConditional %22 %23 %24
         %23 = OpLabel
         %26 = OpAccessChain %_ptr_Function_uint %pixel %uint_1
         %27 = OpLoad %uint %26
         %29 = OpUGreaterThanEqual %bool %27 %uint_600
               OpBranch %24
         %24 = OpLabel
         %30 = OpPhi %bool %21 %5 %29 %23
               OpSelectionMerge %32 None
               OpBranchConditional %30 %31 %32
         %31 = OpLabel
               OpReturn
         %32 = OpLabel
         %42 = OpLoad %39 %motion_image
         %43 = OpLoad %v2uint %pixel
         %46 = OpBitcast %v2int %43
         %48 = OpImage %38 %42
         %49 = OpImageFetch %v4float %48 %46 Sample %int_0
               OpStore %motion %49
         %54 = OpAccessChain %_ptr_Function_uint %pixel %uint_1
         %55 = OpLoad %uint %54
         %56 = OpIMul %uint %55 %uint_1000
         %57 = OpAccessChain %_ptr_Function_uint %pixel %uint_0
         %58 = OpLoad %uint %57
         %59 = OpIAdd %uint %56 %58
         %61 = OpLoad %v4float %motion
         %62 = OpVectorShuffle %v2float %61 %61 0 1
         %63 = OpExtInst %uint %1 PackHalf2x16 %62
         %64 = OpLoad %v4float %motion
         %65 = OpVectorShuffle %v2float %64 %64 2 3
         %66 = OpExtInst %uint %1 PackHalf2x16 %65
         %67 = OpCompositeConstruct %v2uint %63 %66
         %69 = OpAccessChain %_ptr_Uniform_v2uint %_ %int_0 %59
               OpStore %69 %67
               OpReturn
               OpFunctionEnd
#endif

const uint32_t motion_resolve_cs[] = {
    0x07230203, 0x00010000, 0x0008000B, 0x00000048, 0x00000000, 0x00020011, 0x00000001, 0x0006000B,
    0x00000001, 0x4C534C47, 0x6474732E, 0x3035342E, 0x00000000, 0x0003000E, 0x00000000, 0x00000001,
    0x0006000F, 0x00000005, 0x00000004, 0x6E69616D, 0x00000000, 0x0000000C, 0x00060010, 0x00000004,
    0x00000011, 0x00000008, 0x00000008, 0x00000001, 0x00030003, 0x00000002, 0x000001C2, 0x00040005,
    0x00000004, 0x6E69616D, 0x00000000, 0x00040005, 0x00000009, 0x65786970, 0x0000006C, 0x00080005,
    0x0000000C, 0x475F6C67, 0x61626F6C, 0x766E496C, 0x7461636F, 0x496E6F69, 0x00000044, 0x00040005,
    0x00000025, 0x69746F6D, 0x00006E6F, 0x00060005, 0x00000029, 0x69746F6D, 0x695F6E6F, 0x6567616D,
    0x00000000, 0x00050005, 0x00000033, 0x69746F4D, 0x754F6E6F, 0x00000074, 0x00050006, 0x00000033,
    0x00000000, 0x65786970, 0x0000736C, 0x00030005, 0x00000035, 0x00000000, 0x00040047, 0x0000000C,
    0x0000000B, 0x0000001C, 0x00040047, 0x00000029, 0x00000021, 0x00000000, 0x00040047, 0x00000029,
    0x00000022, 0x00000000, 0x00040047, 0x00000032, 0x00000006, 0x00000008, 0x00030047, 0x00000033,
    0x00000003, 0x00040048, 0x00000033, 0x00000000, 0x00000019, 0x00050048, 0x00000033, 0x00000000,
    0x00000023, 0x00000000, 0x00030047, 0x00000035, 0x00000019, 0x00040047, 0x00000035, 0x00000021,
    0x00000001, 0x00040047, 0x00000035, 0x00000022, 0x00000000, 0x00040047, 0x00000047, 0x0000000B,
    0x00000019, 0x00020013, 0x00000002, 0x00030021, 0x00000003, 0x00000002, 0x00040015, 0x00000006,
    0x00000020, 0x00000000, 0x00040017, 0x00000007, 0x00000006, 0x00000002, 0x00040020, 0x00000008,
    0x00000007, 0x00000007, 0x00040017, 0x0000000A, 0x00000006, 0x00000003, 0x00040020, 0x0000000B,
    0x00000001, 0x0000000A, 0x0004003B, 0x0000000B, 0x0000000C, 0x00000001, 0x00020014, 0x0000000F,
    0x0004002B, 0x00000006, 0x00000010, 0x00000000, 0x00040020, 0x00000011, 0x00000007, 0x00000006,
    0x0004002B, 0x00000006, 0x00000014, 0x000003E8, 0x0004002B, 0x00000006, 0x00000019, 0x00000001,
    0x0004002B, 0x00000006, 0x0000001C, 0x00000258, 0x00030016, 0x00000022, 0x00000020, 0x00040017,
    0x00000023, 0x00000022, 0x00000004, 0x00040020, 0x00000024, 0x00000007, 0x00000023, 0x00090019,
    0x00000026, 0x00000022, 0x00000001, 0x00000000, 0x00000000, 0x00000001, 0x00000001, 0x00000000,
    0x0003001B, 0x00000027, 0x00000026, 0x00040020, 0x00000028, 0x00000000, 0x00000027, 0x0004003B,
    0x00000028, 0x00000029, 0x00000000, 0x00040015, 0x0000002C, 0x00000020, 0x00000001, 0x00040017,
    0x0000002D, 0x0000002C, 0x00000002, 0x0004002B, 0x0000002C, 0x0000002F, 0x00000000, 0x0003001D,
    0x00000032, 0x00000007, 0x0003001E, 0x00000033, 0x00000032, 0x00040020, 0x00000034, 0x00000002,
    0x00000033, 0x0004003B, 0x00000034, 0x00000035, 0x00000002, 0x00040017, 0x0000003C, 0x00000022,
    0x00000002, 0x00040020, 0x00000044, 0x00000002, 0x00000007, 0x0004002B, 0x00000006, 0x00000046,
    0x00000008, 0x0006002C, 0x0000000A, 0x00000047, 0x00000046, 0x00000046, 0x00000019, 0x00050036,
    0x00000002, 0x00000004, 0x00000000, 0x00000003, 0x000200F8, 0x00000005, 0x0004003B, 0x00000008,
    0x00000009, 0x00000007, 0x0004003B, 0x00000024, 0x00000025, 0x00000007, 0x0004003D, 0x0000000A,
    0x0000000D, 0x0000000C, 0x0007004F, 0x00000007, 0x0000000E, 0x0000000D, 0x0000000D, 0x00000000,
    0x00000001, 0x0003003E, 0x00000009, 0x0000000E, 0x00050041, 0x00000011, 0x00000012, 0x00000009,
    0x00000010, 0x0004003D, 0x00000006, 0x00000013, 0x00000012, 0x000500AE, 0x0000000F, 0x00000015,
    0x00000013, 0x00000014, 0x000400A8, 0x0000000F, 0x00000016, 0x00000015, 0x000300F7, 0x00000018,
    0x00000000, 0x000400FA, 0x00000016, 0x00000017, 0x00000018, 0x000200F8, 0x00000017, 0x00050041,
    0x00000011, 0x0000001A, 0x00000009, 0x00000019, 0x0004003D, 0x00000006, 0x0000001B, 0x0000001A,
    0x000500AE, 0x0000000F, 0x0000001D, 0x0000001B, 0x0000001C, 0x000200F9, 0x00000018, 0x000200F8,
    0x00000018, 0x000700F5, 0x0000000F, 0x0000001E, 0x00000015, 0x00000005, 0x0000001D, 0x00000017,
    0x000300F7, 0x00000020, 0x00000000, 0x000400FA, 0x0000001E, 0x0000001F, 0x00000020, 0x000200F8,
    0x0000001F, 0x000100FD, 0x000200F8, 0x00000020, 0x0004003D, 0x00000027, 0x0000002A, 0x00000029,
    0x0004003D, 0x00000007, 0x0000002B, 0x00000009, 0x0004007C, 0x0000002D, 0x0000002E, 0x0000002B,
    0x00040064, 0x00000026, 0x00000030, 0x0000002A, 0x0007005F, 0x00000023, 0x00000031, 0x00000030,
    0x0000002E, 0x00000040, 0x0000002F, 0x0003003E, 0x00000025, 0x00000031, 0x00050041, 0x00000011,
    0x00000036, 0x00000009, 0x00000019, 0x0004003D, 0x00000006, 0x00000037, 0x00000036, 0x00050084,
    0x00000006, 0x00000038, 0x00000037, 0x00000014, 0x00050041, 0x00000011, 0x00000039, 0x00000009,
    0x00000010, 0x0004003D, 0x00000006, 0x0000003A, 0x00000039, 0x00050080, 0x00000006, 0x0000003B,
    0x00000038, 0x0000003A, 0x0004003D, 0x00000023, 0x0000003D, 0x00000025, 0x0007004F, 0x0000003C,
    0x0000003E, 0x0000003D, 0x0000003D, 0x00000000, 0x00000001, 0x0006000C, 0x00000006, 0x0000003F,
    0x00000001, 0x0000003A, 0x0000003E, 0x0004003D, 0x00000023, 0x00000040, 0x00000025, 0x0007004F,
    0x0000003C, 0x00000041, 0x00000040, 0x00000040, 0x00000002, 0x00000003, 0x0006000C, 0x00000006,
    0x00000042, 0x00000001, 0x0000003A, 0x00000041, 0x00050050, 0x00000007, 0x00000043, 0x0000003F,
    0x00000042, 0x00060041, 0x00000044, 0x00000045, 0x00000035, 0x0000002F, 0x0000003B, 0x0003003E,
    0x00000045, 0x00000043, 0x000100FD, 0x00010038,
};
