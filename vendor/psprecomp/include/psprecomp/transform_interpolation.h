#ifndef PSPRECOMP_TRANSFORM_INTERPOLATION_H
#define PSPRECOMP_TRANSFORM_INTERPOLATION_H
#include "render_transform.h"
#include <math.h>
#include <string.h>
static inline float psp_pose_dot(const float *a,const float *b){return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];}
static inline void psp_pose_cross(const float *a,const float *b,float *o){o[0]=a[1]*b[2]-a[2]*b[1];o[1]=a[2]*b[0]-a[0]*b[2];o[2]=a[0]*b[1]-a[1]*b[0];}
static inline int psp_pose_normalize(float *v){float n=sqrtf(psp_pose_dot(v,v));if(!isfinite(n)||n<1e-8f)return 0;for(int i=0;i<3;i++)v[i]/=n;return 1;}
static inline int psp_pose_matrix(const float *a,const float *b,float alpha,float *o){
 float axes[3][3],scale[3],sign=1;
 for(int k=0;k<3;k++){
  float x[3],y[3];for(int i=0;i<3;i++){x[i]=a[k*3+i];y[i]=b[k*3+i];}
  float la=sqrtf(psp_pose_dot(x,x)),lb=sqrtf(psp_pose_dot(y,y));
  if(!psp_pose_normalize(x)||!psp_pose_normalize(y)||psp_pose_dot(x,y)<0)return 0;
  scale[k]=la+(lb-la)*alpha;
  for(int i=0;i<3;i++)axes[k][i]=x[i]+(y[i]-x[i])*alpha;
 }
 /* A rigid decomposition cannot preserve shear. Reject it rather than
  * orthogonalizing even identical endpoints into a moving midpoint. */
 for(int k=0;k<3;k++)for(int j=k+1;j<3;j++){
  float aa=psp_pose_dot(a+k*3,a+j*3),bb=psp_pose_dot(b+k*3,b+j*3);
  float al=sqrtf(psp_pose_dot(a+k*3,a+k*3)*psp_pose_dot(a+j*3,a+j*3));
  float bl=sqrtf(psp_pose_dot(b+k*3,b+k*3)*psp_pose_dot(b+j*3,b+j*3));
  if(fabsf(aa)>al*.001f||fabsf(bb)>bl*.001f)return 0;
 }
 float ca[3],cb[3];psp_pose_cross(a,a+3,ca);psp_pose_cross(b,b+3,cb);
 float da=psp_pose_dot(ca,a+6),db=psp_pose_dot(cb,b+6);
 if(da*db<=0)return 0;if(da<0)sign=-1;
 if(!psp_pose_normalize(axes[0]))return 0;
 float d=psp_pose_dot(axes[0],axes[1]);for(int i=0;i<3;i++)axes[1][i]-=d*axes[0][i];
 if(!psp_pose_normalize(axes[1]))return 0;psp_pose_cross(axes[0],axes[1],axes[2]);
 for(int k=0;k<3;k++)for(int i=0;i<3;i++)o[k*3+i]=axes[k][i]*scale[k]*(k==2?sign:1);
 for(int i=9;i<12;i++)o[i]=a[i]+(b[i]-a[i])*alpha;
 return 1;
}
static inline int psp_transform_between(const psp_render_transform *a,const psp_render_transform *b,float alpha,psp_render_transform *out){
 if(!a||!b||!out||!a->valid||!b->valid||!isfinite(alpha)||alpha<0||alpha>1)return 0;
 const float *av[]={a->world,a->view,a->proj,a->viewport},*bv[]={b->world,b->view,b->proj,b->viewport};
 const int n[]={12,12,16,6};
 for(int k=0;k<4;k++)for(int i=0;i<n[k];i++)if(!isfinite(av[k][i])||!isfinite(bv[k][i]))return 0;
 if(memcmp(a->viewport,b->viewport,sizeof a->viewport))return 0;
 if(memcmp(a->proj,b->proj,sizeof a->proj)){
  /* Smooth speed zoom retains the perspective shape, aspect and depth.
   * A different camera/projection, aspect or abrupt FOV step still snaps. */
  if(a->proj[15]!=0||fabsf(a->proj[11])<.5f||a->proj[0]*b->proj[0]<=0||a->proj[5]*b->proj[5]<=0)return 0;
  for(int i=0;i<16;i++)if(i!=0&&i!=5&&a->proj[i]!=b->proj[i])return 0;
  float x=b->proj[0]/a->proj[0],y=b->proj[5]/a->proj[5];
  if(fabsf(x-1)>.02f||fabsf(y-1)>.02f||fabsf(x-y)>.0001f)return 0;
 }
 *out=*b;
 if(alpha==0){memcpy(out->world,a->world,sizeof a->world);memcpy(out->view,a->view,sizeof a->view);memcpy(out->proj,a->proj,sizeof a->proj);return 1;}
 if(alpha==1)return 1;
 for(int i=0;i<16;i++)out->proj[i]=a->proj[i]+(b->proj[i]-a->proj[i])*alpha;
 return psp_pose_matrix(a->world,b->world,alpha,out->world)&&psp_pose_matrix(a->view,b->view,alpha,out->view);
}
static inline int psp_transform_project_depth(const psp_render_transform *t,const float *local,int width,int height,float *out,float *raw_depth){
 if(!t||!t->valid||!local||!out||width<=0||height<=0)return 0;
 float w[3],v[3],c[4];
 for(int r=0;r<3;r++)w[r]=t->world[r]*local[0]+t->world[3+r]*local[1]+t->world[6+r]*local[2]+t->world[9+r];
 for(int r=0;r<3;r++)v[r]=t->view[r]*w[0]+t->view[3+r]*w[1]+t->view[6+r]*w[2]+t->view[9+r];
 for(int r=0;r<4;r++)c[r]=t->proj[r]*v[0]+t->proj[4+r]*v[1]+t->proj[8+r]*v[2]+t->proj[12+r];
 for(int i=0;i<4;i++)if(!isfinite(c[i]))return 0;
 if(c[3]<=1e-6f||c[2]+c[3]<0)return 0;
 const float *p=t->viewport;
 out[0]=((c[0]/c[3]*p[0]+p[1])/width*2-1)*c[3];
 out[1]=((c[1]/c[3]*p[2]+p[3])/height*2-1)*c[3];
 float depth=c[2]/c[3]*p[4]+p[5];
 if(raw_depth)*raw_depth=depth;
 out[2]=(depth/65535.0f*2-1)*c[3];out[3]=c[3];
 for(int i=0;i<4;i++)if(!isfinite(out[i]))return 0;
 return 1;
}
/* Keep the established projection API for callers that need clip position only. */
static inline int psp_transform_project(const psp_render_transform *t,const float *local,int width,int height,float *out){
 return psp_transform_project_depth(t,local,width,height,out,(float *)0);
}
#endif
