/// Outputs white, which the blend function turns into a multiplication of the target by the blend colour.
#version 330 core
out vec4 colour;
void main()
{
    colour = vec4(1.0);
}
