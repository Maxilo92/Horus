#ifndef VIDEO_RENDERER_HPP
#define VIDEO_RENDERER_HPP

#include <GL/glew.h>
#include <opencv2/opencv.hpp>
#include "imgui.h"

class VideoRenderer {
public:
    VideoRenderer();
    ~VideoRenderer();

    void updateTexture(const cv::Mat& frame);
    void drawBackground(int windowWidth, int windowHeight, const cv::Mat& frame);
    GLuint getTextureID() const { return m_textureID; }

private:
    GLuint m_textureID;
};

#endif // VIDEO_RENDERER_HPP
