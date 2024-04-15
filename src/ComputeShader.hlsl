#define blocksize 128

static float softening_squared = 0.0012500000f * 0.0012500000f;
static float scale_factor = 10000.0f;
static float G = 6.67300e-11f * scale_factor;
static float particle_mass = scale_factor * scale_factor;

groupshared float4 updated_positions[blocksize];

void calculate_acceleration(inout float3 a_i, float4 p_j, float4 p_i, float mass, int particles = 1)
{
    float3 r = p_j.xyz - p_i.xyz;
    float dist = sqrt(dot(r, r) + softening_squared);
    
    float F = G * mass * particles / (dist * dist * dist); // dist_cube to slow the simulation down
    a_i += r * F;
}

cbuffer cbCS : register(b0)
{
    uint4 param;    // param[0] - max particles
                    // param[1] - dimx
    
    float4 paramf;
};

struct particle_t
{
    float4 position;
    float4 velocity;
};

StructuredBuffer<particle_t> particle_data : register(t0); // SRV
RWStructuredBuffer<particle_t> updated_particle_data : register(u0); // UAV

[numthreads(blocksize, 1, 1)]
void main(uint3 g_id : SV_GroupID, uint3 DT_id : SV_DispatchThreadID, uint3 GT_id : SV_GroupThreadID, uint GI : SV_GroupIndex)
{
    float4 current_position = particle_data[DT_id.x].position;
    float4 current_velocity = particle_data[DT_id.x].velocity;
    float3 a = 0;
    float mass = particle_mass;
    
    uint neighbors = param.y;
    
    [loop]
    for (uint i = 0; i < neighbors; ++i)
    {
        updated_positions[GI] = particle_data[i * blocksize + GI].position;
        
        GroupMemoryBarrierWithGroupSync();
        
        [unroll]
        for (uint counter = 0; counter < blocksize; counter += 8) 
        {
            calculate_acceleration(a, updated_positions[counter + 0], current_position, mass);
            calculate_acceleration(a, updated_positions[counter + 1], current_position, mass);
            calculate_acceleration(a, updated_positions[counter + 2], current_position, mass);
            calculate_acceleration(a, updated_positions[counter + 3], current_position, mass);
            calculate_acceleration(a, updated_positions[counter + 4], current_position, mass);
            calculate_acceleration(a, updated_positions[counter + 5], current_position, mass);
            calculate_acceleration(a, updated_positions[counter + 6], current_position, mass);
            calculate_acceleration(a, updated_positions[counter + 7], current_position, mass);
        }
        GroupMemoryBarrierWithGroupSync();
    }

    // what if the amount of particles is not an exact multiple of the tile size? 
    // this might lead into generating false gravity at position (0, 0, 0), so
    // we have to subtract them
    // const int tooManyParticles = param.y * blocksize - param.x;
    // calculate_acceleration(a, float4(0.0, 0.0, 0.0, 0.0), current_position, mass, -tooManyParticles);
    
    float damping = paramf.y;
    float delta_time = paramf.x;
    
    current_velocity.xyz += a.xyz * delta_time;
    current_velocity.xyz *= damping;
    
    current_position.xyz += current_velocity.xyz * paramf.x;
    
    if (DT_id.x < param.x)
    {
        updated_particle_data[DT_id.x].position = current_position;
        updated_particle_data[DT_id.x].velocity = float4(current_velocity.xyz, length(a));
    }
}