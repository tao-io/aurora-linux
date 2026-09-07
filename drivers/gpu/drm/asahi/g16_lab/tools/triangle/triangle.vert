#version 450
layout(location = 0) in vec4 vertex;
layout(location = 0) noperspective out vec3 rgb;
void main()
{
    gl_Position = vec4(vertex.xy, 0.0, 1.0);
    rgb = vec3(vertex.zw, 1.0 - vertex.z - vertex.w);
}
