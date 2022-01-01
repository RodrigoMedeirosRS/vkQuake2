#version 450
#extension GL_ARB_separate_shader_objects : enable

layout(push_constant) uniform PushConstant
{
	float postprocess;
	float gamma;
	float offsetmultiplier;
	float blur;
	float resolutionx;
	float resolutiony;
} pc;

layout(set = 0, binding = 0) uniform sampler2D sTexture;
layout(location = 0) in vec2 texCoord;
layout(location = 0) out vec4 fragmentColor;

void main() 
{
	if (pc.postprocess > 0.0 && pc.resolutionx != 0)
	{
		float intensity = 1;
		float blurSize = 1.0/pc.resolutiony;
		vec4 sum = vec4(0);
   		vec2 texcoordBloom = vec2(texCoord.x/pc.resolutionx, texCoord.y/pc.resolutiony);
		int j = 0;
   		int i = 0;

		float distanceToCenter = distance(texCoord, vec2(0.5, 0.5));
		float distortionAmount = 1.5 * distanceToCenter * float(2);
    	float dist = float(distance(texCoord, vec2(0,5)) * 0.001);

		sum += texture(sTexture, vec2(texcoordBloom.x - 4.0*blurSize, texcoordBloom.y)) * 0.05;
   		sum += texture(sTexture, vec2(texcoordBloom.x - 3.0*blurSize, texcoordBloom.y)) * 0.09;
		sum += texture(sTexture, vec2(texcoordBloom.x - 2.0*blurSize, texcoordBloom.y)) * 0.12;
		sum += texture(sTexture, vec2(texcoordBloom.x - blurSize, texcoordBloom.y)) * 0.15;
		sum += texture(sTexture, vec2(texcoordBloom.x, texcoordBloom.y)) * 0.16;
		sum += texture(sTexture, vec2(texcoordBloom.x + blurSize, texcoordBloom.y)) * 0.15;
		sum += texture(sTexture, vec2(texcoordBloom.x + 2.0*blurSize, texcoordBloom.y)) * 0.12;
		sum += texture(sTexture, vec2(texcoordBloom.x + 3.0*blurSize, texcoordBloom.y)) * 0.09;
		sum += texture(sTexture, vec2(texcoordBloom.x + 4.0*blurSize, texcoordBloom.y)) * 0.05;

		sum += texture(sTexture, vec2(texcoordBloom.x, texcoordBloom.y - 4.0*blurSize)) * 0.05;
		sum += texture(sTexture, vec2(texcoordBloom.x, texcoordBloom.y - 3.0*blurSize)) * 0.09;
		sum += texture(sTexture, vec2(texcoordBloom.x, texcoordBloom.y - 2.0*blurSize)) * 0.12;
		sum += texture(sTexture, vec2(texcoordBloom.x, texcoordBloom.y - blurSize)) * 0.15;
		sum += texture(sTexture, vec2(texcoordBloom.x, texcoordBloom.y)) * 0.16;
		sum += texture(sTexture, vec2(texcoordBloom.x, texcoordBloom.y + blurSize)) * 0.15;
		sum += texture(sTexture, vec2(texcoordBloom.x, texcoordBloom.y + 2.0*blurSize)) * 0.12;
		sum += texture(sTexture, vec2(texcoordBloom.x, texcoordBloom.y + 3.0*blurSize)) * 0.09;
		sum += texture(sTexture, vec2(texcoordBloom.x, texcoordBloom.y + 4.0*blurSize)) * 0.05;

		fragmentColor.r = sum.r * intensity + pow(texture(sTexture,texCoord + vec2(distortionAmount * dist, 0.0), 1.5).r * 1.5, pc.gamma);  
		fragmentColor.g = sum.g * intensity + pow(texture(sTexture,texCoord, 1.5).g * 1.5, pc.gamma);
		fragmentColor.b = sum.b * intensity + pow(texture(sTexture,texCoord - vec2(distortionAmount * dist, 0.0), 1.5).b * 1.5, pc.gamma);

		//fragmentColor.r = pow(texture(sTexture,texCoord + vec2(distortionAmount * dist, 0.0), pc.blur).r * 1.5, pc.gamma);  
		//fragmentColor.g = pow(texture(sTexture,texCoord, pc.blur).g * 1.5, pc.gamma);
		//fragmentColor.b = pow(texture(sTexture,texCoord - vec2(distortionAmount * dist, 0.0), pc.blur).b * 1.5, pc.gamma);
	}
	else
	{
		fragmentColor = texture(sTexture, texCoord);
	}
}
