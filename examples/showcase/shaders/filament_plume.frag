#version 450

layout(location = 0) out vec4 out_color;

void main()
{
    /* p5.js stroke(400, 96): white at 96/255 opacity.  Flux's PREMUL
     * blend preset expects RGB to be premultiplied by alpha. */
    const float alpha = 96.0 / 255.0;
    out_color = vec4(alpha, alpha, alpha, alpha);
}
