#include "VideoRenderer.hpp"

VideoRenderer::VideoRenderer() : m_textureID(0) {}

VideoRenderer::~VideoRenderer() {
    if (m_textureID != 0) {
        glDeleteTextures(1, &m_textureID);
    }
}

void VideoRenderer::updateTexture(const cv::Mat& frame) {
    if (frame.empty()) return;

    if (m_textureID == 0) {
        glGenTextures(1, &m_textureID);
    }

    glBindTexture(GL_TEXTURE_2D, m_textureID);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // Configure unpacking alignment and row length based on cv::Mat's properties
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, frame.step / frame.elemSize());

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, frame.cols, frame.rows, 0, GL_BGR, GL_UNSIGNED_BYTE, frame.data);

    // Restore default unpack alignment and row length to avoid affecting other uploads
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
}

void VideoRenderer::drawBackground(int windowWidth, int windowHeight, const cv::Mat& frame) {
    if (m_textureID == 0 || frame.empty()) return;

    float frame_aspect = (float)frame.cols / (float)frame.rows;
    float window_aspect = (float)windowWidth / (float)windowHeight;
    
    float target_w, target_h;
    float pos_x = 0, pos_y = 0;

    if (window_aspect > frame_aspect) {
        target_h = (float)windowHeight;
        target_w = target_h * frame_aspect;
        pos_x = (windowWidth - target_w) / 2.0f;
    } else {
        target_w = (float)windowWidth;
        target_h = target_w / frame_aspect;
        pos_y = (windowHeight - target_h) / 2.0f;
    }

    ImGui::GetBackgroundDrawList()->AddImage(
        (void*)(intptr_t)m_textureID, 
        ImVec2(pos_x, pos_y), 
        ImVec2(pos_x + target_w, pos_y + target_h)
    );
}
