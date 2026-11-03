#version 450
layout(location = 0) in vec4 pos;
layout(location = 4) in vec4 col;
layout(location = 0) out vec4 vcol;
void main() {
    gl_Position = pos;
    vcol = col;
}
