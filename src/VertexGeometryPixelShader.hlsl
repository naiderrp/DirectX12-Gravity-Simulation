struct VertexInput
{
    float4 color : COLOR;
    uint id : SV_VERTEXID;
};

struct VertexOutput
{
    float3 pos : POSITION;
    float4 color : COLOR;
};

struct GeometryOutput
{
    float2 tex : TEXCOORD0;
    float4 color : COLOR;
    float4 pos : SV_POSITION;
};

struct PixelInput
{
    float2 tex : TEXCOORD0;
    float4 color : COLOR;
};

struct particle_t
{
    float4 position;
    float4 velocity;
};

StructuredBuffer<particle_t> particle_data;

cbuffer cb0
{
    row_major matrix mvp;
    row_major matrix inv_view;
};

cbuffer cb1
{
    static float particle_radius = 10.0f;
};

cbuffer cbImmutable
{
    static float3 g_positions[4] =
    {
        float3(-1, 1, 0),
        float3(1, 1, 0),
        float3(-1, -1, 0),
        float3(1, -1, 0),
    };
    
    static float2 g_texcoords[4] =
    {
        float2(0, 0),
        float2(1, 0),
        float2(0, 1),
        float2(1, 1),
    };
};

// drawing the point-sprite particles

VertexOutput VS_main(VertexInput IN)
{
    VertexOutput OUT;
    
    OUT.pos = particle_data[IN.id].position.xyz;
    
    float magnitude = particle_data[IN.id].velocity.w / 9.0f;
    float4 red = float4(1.0f, 0.1f, 0.1f, 1.0f);
    float4 cyan_blue = float4(0.0f, 1.0f, 1.0f, 1.0f);
    
    OUT.color = lerp(cyan_blue, IN.color, magnitude);
    
    return OUT;
}

// turn abort point into 2 triangles

[maxvertexcount(4)]
void GS_main(point VertexOutput IN[1], inout TriangleStream<GeometryOutput> sprite_stream)
{
    GeometryOutput OUT;
    
    for (int i = 0; i < 4; ++i)
    {
        float3 position = g_positions[i] * particle_radius;
        position = mul(position, (float3x3) inv_view) + IN[0].pos;
        OUT.pos = mul(float4(position, 1.0), mvp);

        OUT.color = IN[0].color;
        OUT.tex = g_texcoords[i];
        sprite_stream.Append(OUT);
    }
    
    sprite_stream.RestartStrip();
}

float4 PS_main(PixelInput IN) : SV_Target
{
    float intensity = 0.5f - length(float2(0.5f, 0.5f) - IN.tex);
    intensity = clamp(intensity, 0.0f, 0.5f) * 2.0f;
    
    return float4(IN.color.xyz, intensity);
}