#ifndef __COMMON_HLSL__
#define __COMMON_HLSL__

struct Frustum
{
    float4 planes[6];
};

cbuffer PerFrame : register(b0)
{
    float4x4 viewProjMatrix;
    float4x4 viewMatrix;
    float4x4 projMatrix;
    float4x4 invViewProjMatrix;
    float4x4 invViewMatrix;
    float4x4 invProjMatrix;
    Frustum mainViewFrustum;
};

SamplerState pointSampler : register(s0);
SamplerState linearSingleMipSampler : register(s1);
SamplerState linearSampler : register(s2);
SamplerState anisoSampler : register(s3);
SamplerComparisonState shadowSampler : register(s4);

Texture2D<float4> sceneTextures[300] : register(t0, space1);

#endif
