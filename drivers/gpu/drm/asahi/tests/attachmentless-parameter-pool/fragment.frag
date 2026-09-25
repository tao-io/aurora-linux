#version 450
layout(set=0,binding=0,std430) buffer Data { uint values[]; } data;
void main(){ uint i=uint(gl_FragCoord.y)*64+uint(gl_FragCoord.x);data.values[i]=i+1; }
