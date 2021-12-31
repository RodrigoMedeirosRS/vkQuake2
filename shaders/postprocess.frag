#version 450
#extension GL_ARB_separate_shader_objects : enable

layout(push_constant) uniform PushConstant
{
	float postprocess;
	float gamma;
	float offsetmultiplier;
	float blur;
} pc;

layout(set = 0, binding = 0) uniform sampler2D sTexture;

layout(location = 0) in vec2 texCoord;
layout(location = 0) out vec4 fragmentColor;

void main() 
{
	if (pc.postprocess > 0.0)
	{
		float distanceToCenter = distance(texCoord, vec2(0.5, 0.5));
		//float distortionAmount = pc.offsetmultiplier * distanceToCenter * float(2);
		float distortionAmount = 1 * distanceToCenter * float(2);
    	float dist = float(distance(texCoord, vec2(0,5)) * 0.001);
    
		fragmentColor.r = pow(texture(sTexture,texCoord + vec2(distortionAmount * dist, 0.0), 1.5).r * 1.5, pc.gamma);  
		fragmentColor.g = pow(texture(sTexture,texCoord, 1.5).g * 1.5, pc.gamma);
		fragmentColor.b = pow(texture(sTexture,texCoord - vec2(distortionAmount * dist, 0.0), 1.5).b * 1.5, pc.gamma);

		//fragmentColor.r = pow(texture(sTexture,texCoord + vec2(distortionAmount * dist, 0.0), pc.blur).r * 1.5, pc.gamma);  
		//fragmentColor.g = pow(texture(sTexture,texCoord, pc.blur).g * 1.5, pc.gamma);
		//fragmentColor.b = pow(texture(sTexture,texCoord - vec2(distortionAmount * dist, 0.0), pc.blur).b * 1.5, pc.gamma);
	}
	else
	{
		fragmentColor = texture(sTexture, texCoord);
	}
}
