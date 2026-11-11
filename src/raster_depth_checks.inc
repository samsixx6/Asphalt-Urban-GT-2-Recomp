/* Test-only raster/depth contract: appended only to a private renderer build.
 * The production VS_DRAW and FS_DRAW strings and gvert ABI are used directly.
 * No title-specific offsets or scene inspection are needed. */
static GLuint audit_fbo[3], audit_tex[2], audit_depth[3];
static void audit_triangle(float depth, uint32_t rgba, float w0,float w1,float w2) {
    gvert v[3];memset(v,0,sizeof v);
    const float xy[3][2]={{-1,-1},{3,-1},{-1,3}},w[3]={w0,w1,w2};
    for(int i=0;i<3;i++) {
        v[i].x=xy[i][0]*w[i];v[i].y=xy[i][1]*w[i];
        v[i].z=(depth/65535.0f*2.0f-1.0f)*w[i];v[i].w=w[i];
        v[i].depth=v[i].depth_base=depth;v[i].rgba=rgba;v[i].fog=1;
    }
    glBindVertexArray(g_vao);glBindBuffer(GL_ARRAY_BUFFER,g_vbo);
    glBufferData(GL_ARRAY_BUFFER,sizeof v,v,GL_STREAM_DRAW);glDrawArrays(GL_TRIANGLES,0,3);
}
static uint32_t audit_pixel(void) {
    uint8_t p[4]={0};glReadPixels(8,8,1,1,GL_RGBA,GL_UNSIGNED_BYTE,p);
    return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16;
}
static void audit_base(GLuint fbo,GLuint shader) {
    glBindFramebuffer(GL_FRAMEBUFFER,fbo);glViewport(0,0,16,16);
    glUseProgram(shader);glDisable(GL_SCISSOR_TEST);glDisable(GL_BLEND);
    glDisable(GL_STENCIL_TEST);glDisable(GL_DEPTH_TEST);glColorMask(1,1,1,1);glDepthMask(1);
    if(shader==g_prog) {
        glUniform4i(g_u_mode,0,0,0,0);glUniform3f(g_u_source_factor,1,1,1);
    }
}
static void audit_initial_depth(GLenum compare,float initial) {
    audit_base(audit_fbo[0],g_prog);glEnable(GL_DEPTH_TEST);glDepthFunc(GL_ALWAYS);
    audit_triangle(initial,0xff000000u,1,2,4);glDepthFunc(compare);
}
static int audit_occlusion(GLenum compare,int reverse) {
    const float nearz=compare==GL_GEQUAL?6200.0f:2800.0f;
    const float farz=compare==GL_GEQUAL?2800.0f:6200.0f;
    audit_initial_depth(compare,compare==GL_GEQUAL?0.0f:65535.0f);
    if(reverse) {
        audit_triangle(farz,0xffff0000u,1,2,4);audit_triangle(nearz,0xff0000ffu,1,2,4);
    } else {
        audit_triangle(nearz,0xff0000ffu,1,2,4);audit_triangle(farz,0xffff0000u,1,2,4);
    }
    uint32_t got=audit_pixel();int fail=got!=0xffu;
    fprintf(stderr,"audit final_depth func=%X reverse=%d actual=%06X expected=0000FF result=%s\n",compare,reverse,got,fail?"RED":"PASS");
    return fail;
}
static int audit_gpu_contract(void) {
    int failed=0,raster_red=0;GLuint raster=program(VS_DRAW,
        "out vec4 o_col;void main(){uint q=uint(clamp(gl_FragCoord.z,0.0,1.0)*65535.0+0.5);"
        "o_col=vec4(float(q&255u)/255.0,float(q>>8)/255.0,0.0,1.0);}");
    GLuint hazard=program(VS_DRAW,
        "out vec4 o_col;void main(){if(gl_FragCoord.z<(6200.0/65535.0))discard;o_col=vec4(0,0,1,1);}");
    if(!raster||!hazard){fprintf(stderr,"audit program failed\n");return 1;}
    for(int i=0;i<3;i++) {
        glGenRenderbuffers(1,&audit_depth[i]);glBindRenderbuffer(GL_RENDERBUFFER,audit_depth[i]);
        glRenderbufferStorage(GL_RENDERBUFFER,GL_DEPTH24_STENCIL8,16,16);
        glGenFramebuffers(1,&audit_fbo[i]);glBindFramebuffer(GL_FRAMEBUFFER,audit_fbo[i]);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER,GL_DEPTH_STENCIL_ATTACHMENT,GL_RENDERBUFFER,audit_depth[i]);
        if(i<2) {
            audit_tex[i]=new_texture(16,16,NULL,GL_NEAREST);
            glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,audit_tex[i],0);
        } else { const GLenum none=GL_ZERO;glDrawBuffers(1,&none);glReadBuffer(GL_ZERO); }
        if(glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE){fprintf(stderr,"audit incomplete_fbo=%d\n",i);return 1;}
    }
    const float raw[]={0,2755,6239,32767,65535,65535.5f};
    for(unsigned i=0;i<sizeof raw/sizeof raw[0];i++) {
        audit_base(audit_fbo[0],raster);audit_triangle(raw[i],0xffffffffu,1,2,4);
        unsigned got=audit_pixel()&65535,want=(unsigned)(fminf(raw[i],65535)+.5f);
        int delta=(int)got-(int)want;if(abs(delta)>1)raster_red++;
        fprintf(stderr,"audit raster_depth raw=%.4f actual=%u expected=%u delta=%d result=%s\n",raw[i],got,want,delta,abs(delta)>1?"RED":"PASS");
    }
    failed+=audit_occlusion(GL_GEQUAL,0)+audit_occlusion(GL_GEQUAL,1)+audit_occlusion(GL_LESS,0)+audit_occlusion(GL_LESS,1);
    /* Explicitly model the forbidden raster-depth early comparison. This
     * does not claim that a particular driver performs that comparison. */
    audit_base(audit_fbo[0],g_prog);audit_triangle(6200,0xff0000ffu,1,2,4);
    glUseProgram(hazard);audit_triangle(2800,0xffff0000u,1,2,4);
    uint32_t got=audit_pixel();
    fprintf(stderr,"audit simulated_raster_early_gequal actual=%06X expected=0000FF result=%s\n",got,got==255?"PASS":"RED");
    failed+=got!=255;
    /* Real depth-only packed D24S8 round trip through a colorless logical
     * depth FBO. A stencil sentinel verifies that depth copies preserve it. */
    audit_initial_depth(GL_GEQUAL,0);glEnable(GL_STENCIL_TEST);glStencilMask(255);
    glStencilFunc(GL_ALWAYS,7,255);glStencilOp(GL_REPLACE,GL_REPLACE,GL_REPLACE);
    audit_triangle(6200,0xff0000ffu,1,2,4);
    audit_base(audit_fbo[1],g_prog);glEnable(GL_STENCIL_TEST);glStencilMask(255);
    glStencilFunc(GL_ALWAYS,3,255);glStencilOp(GL_REPLACE,GL_REPLACE,GL_REPLACE);
    audit_triangle(0,0xff000000u,1,2,4);
    depth_blit(audit_fbo[0],audit_fbo[2],16,16);depth_blit(audit_fbo[2],audit_fbo[1],16,16);
    audit_base(audit_fbo[1],g_prog);glEnable(GL_DEPTH_TEST);glDepthFunc(GL_GEQUAL);
    glEnable(GL_STENCIL_TEST);glStencilMask(0);glStencilFunc(GL_EQUAL,3,255);glStencilOp(GL_KEEP,GL_KEEP,GL_KEEP);
    audit_triangle(2800,0xffff0000u,1,2,4);got=audit_pixel();failed+=got!=0;
    fprintf(stderr,"audit packed_depth_blit_far actual=%06X expected=000000 result=%s\n",got,got==0?"PASS":"RED");
    audit_triangle(7000,0xff00ff00u,1,2,4);got=audit_pixel();failed+=got!=0xff00;
    fprintf(stderr,"audit packed_depth_blit_near_stencil actual=%06X expected=00FF00 result=%s\n",got,got==0xff00?"PASS":"RED");
    GLenum error=glGetError();failed+=error!=0;
    fprintf(stderr,"audit summary pipeline_checks_failed=%d raster_contract_red=%d gl_error=%X\n",failed,raster_red,error);
    glBindFramebuffer(GL_FRAMEBUFFER,0);glDisable(GL_DEPTH_TEST);glDisable(GL_STENCIL_TEST);
    glDeleteFramebuffers(3,audit_fbo);glDeleteRenderbuffers(3,audit_depth);glDeleteTextures(2,audit_tex);
    glUseProgram(g_prog);glBindVertexArray(g_vao);glBindBuffer(GL_ARRAY_BUFFER,g_vbo);
    return failed+raster_red;
}
