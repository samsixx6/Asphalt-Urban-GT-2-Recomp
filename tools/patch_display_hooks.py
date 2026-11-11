"""Checked hooks in derived C only. Original game image/source stays untouched."""
import hashlib,json
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def patch_source(source,addresses,expected_sha256,native_task_dispatch=False,semantic_prims=False,semantic_components=False,options_selection=False):
 if hashlib.sha256(source.encode()).hexdigest()!=expected_sha256:raise ValueError('Display hook source hash differs')
 for address in addresses:
  address=address.upper()
  old=f'void psp_func_{address}(void) {{ psp_body_{address}(0x{address}u); }}'
  if source.count(old)!=1:raise ValueError('Display hook wrapper count differs: '+address)
  new=f'''void psp_func_{address}(void) {{
    uint32_t object = r_a0;
    if (!asphalt_display_hook_enter(0x{address}u, object)) {{
        psp_body_{address}(0x{address}u);
        asphalt_display_hook_leave(0x{address}u, object);
    }}
}}'''
  source=source.replace(old,new)
 semantic_declaration=''
 if semantic_prims:
  for instruction,store,call in [
   ('08808508  sw         $a2, 0($a0)','psp_write32(r_a0 + 0, r_a2);','asphalt_semantic_prim(r_a0,r_a2);'),
   ('088085E8  sw         $v0, 0($a1)','psp_write32(r_a1 + 0, r_v0);','asphalt_semantic_prim(r_a1,r_v0);')]:
   old='    /* '+instruction+' */\n    '+store
   if source.count(old)!=1:raise ValueError('Semantic PRIM store differs: '+instruction)
   source=source.replace(old,old+'\n    '+call)
  semantic_declaration+='void asphalt_semantic_prim(uint32_t,uint32_t);\n'
 if semantic_components:
  for address in (semantic_components if isinstance(semantic_components,list) else ['088A2318','088A2410','088A250C','088A2604','088A26C8']):
   old=f'    PSP_CALL_BEGIN(0x{address}u);\n    psp_dispatch(_t);\n    PSP_CALL_END(0x{address}u);'
   if source.count(old)!=1:raise ValueError('Semantic component site differs: '+address)
   new=f'    PSP_CALL_BEGIN(0x{address}u);\n    asphalt_semantic_component_begin(r_a0);\n    psp_dispatch(_t);\n    asphalt_semantic_component_end();\n    PSP_CALL_END(0x{address}u);'
   source=source.replace(old,new)
  semantic_declaration+='void asphalt_semantic_component_begin(uint32_t);\nvoid asphalt_semantic_component_end(void);\n'
 native_declaration=''
 if native_task_dispatch:
  old='    PSP_CALL_BEGIN(0x08879ECCu);\n    psp_dispatch(_t);\n    PSP_CALL_END(0x08879ECCu);'
  if source.count(old)!=1:raise ValueError('Native task dispatch site count differs')
  new='    PSP_CALL_BEGIN(0x08879ECCu);\n    if (!asphalt_native_engine_task(_t, r_a0, r_a1, r_a2, r_s0, r_s5)) psp_dispatch(_t);\n    PSP_CALL_END(0x08879ECCu);'
  source=source.replace(old,new)
  native_declaration='int asphalt_native_engine_task(uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t);\n'
 options_declaration=''
 if options_selection:
  old='    /* 08AC1290  lw         $a1, 328($v1) */\n    r_a1 = psp_read32(r_v1 + 328);'
  if source.count(old)!=1:raise ValueError('Options activation selection site differs')
  new='    /* 08AC1290  lw         $a1, 328($v1) */\n    r_a1 = asphalt_display_options_original_id(r_s0, psp_read32(r_v1 + 328));'
  source=source.replace(old,new)
  options_declaration='uint32_t asphalt_display_options_original_id(uint32_t,uint32_t);\n'
 return ('#include <stdint.h>\nint asphalt_display_hook_enter(uint32_t,uint32_t);\n'
  'void asphalt_display_hook_leave(uint32_t,uint32_t);\n'+native_declaration+semantic_declaration+options_declaration+source)
def prepare():
 profile=json.loads((ROOT/'src/display-hooks.json').read_text())
 if hashlib.sha256((ROOT/'work/input/ASPHALT_REBASED.ELF').read_bytes()).hexdigest()!=profile['image_sha256']:raise ValueError('Unsupported game image')
 out=ROOT/'build/display-generated';out.mkdir(parents=True,exist_ok=True);overrides={}
 for name,item in profile['units'].items():
  source=ROOT/'work/generated-ctors'/name
  patched=patch_source(source.read_text(encoding='utf-8'),item['addresses'],item['sha256'],item.get('native_task_dispatch',False),item.get('semantic_prims',False),item.get('semantic_components',False),item.get('options_selection',False))
  target=out/name;target.write_text(patched,encoding='utf-8');overrides[name]=target
 return overrides
if __name__=='__main__':print(json.dumps({k:str(v) for k,v in prepare().items()},indent=2))
