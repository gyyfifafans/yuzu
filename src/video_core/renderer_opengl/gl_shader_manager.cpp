// Copyright 2018 yuzu Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "core/core.h"
#include "core/hle/kernel/process.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/renderer_opengl/gl_shader_manager.h"

namespace GLShader {

namespace Impl {
void SetShaderUniformBlockBinding(GLuint shader, const char* name,
                                  Maxwell3D::Regs::ShaderStage binding, size_t expected_size) {
    GLuint ub_index = glGetUniformBlockIndex(shader, name);
    if (ub_index != GL_INVALID_INDEX) {
        GLint ub_size = 0;
        glGetActiveUniformBlockiv(shader, ub_index, GL_UNIFORM_BLOCK_DATA_SIZE, &ub_size);
        ASSERT_MSG(ub_size == expected_size,
                   "Uniform block size did not match! Got {}, expected {}",
                   static_cast<int>(ub_size), expected_size);
        glUniformBlockBinding(shader, ub_index, static_cast<GLuint>(binding));
    }
}

void SetShaderUniformBlockBindings(GLuint shader) {
    SetShaderUniformBlockBinding(shader, "vs_config", Maxwell3D::Regs::ShaderStage::Vertex,
                                 sizeof(MaxwellUniformData));
    SetShaderUniformBlockBinding(shader, "gs_config", Maxwell3D::Regs::ShaderStage::Geometry,
                                 sizeof(MaxwellUniformData));
    SetShaderUniformBlockBinding(shader, "fs_config", Maxwell3D::Regs::ShaderStage::Fragment,
                                 sizeof(MaxwellUniformData));
}

} // namespace Impl

void MaxwellUniformData::SetFromRegs(const Maxwell3D::State::ShaderStageInfo& shader_stage) {
    const auto& regs = Core::System::GetInstance().GPU().Maxwell3D().regs;

    // TODO(bunnei): Support more than one viewport
    viewport_flip[0] = regs.viewport_transform[0].scale_x < 0.0 ? -1.0f : 1.0f;
    viewport_flip[1] = regs.viewport_transform[0].scale_y < 0.0 ? -1.0f : 1.0f;
}

} // namespace GLShader
