#define AS_RESOLVE_PASS
//#include "ComputeBindings.hlsli"

[numthreads(4, 4, 4)] // Morton hashmap has 512^3 data, dispatch is (128, 128, 128)
void main( uint3 DTid : SV_DispatchThreadID )
{ 
    // Currently kind of a zombie shader ^_^' realized hashmap approach wouldn't work after seeing my memory constraints
    return;
}