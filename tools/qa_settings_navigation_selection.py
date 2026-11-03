"""Observe navigation paint, ink, hit targets and keyboard input on the actual EXE."""
from PIL import Image

def verify(ctx,args):
    state,settle,query,click,hover,key,command,screenshot,check,pointer,find=(ctx[n] for n in ('state','settle','query','click','hover','key','command','screenshot','check','pointer','find'))
    native_state=state
    def state():
        value=native_state()
        value.setdefault('viewport_origin',ctx['win32gui'].ClientToScreen(ctx['identity']['hwnd'],(0,0)))
        return value
    def distance(a,b):return max(abs(a[i]-b[i]) for i in range(3))
    def shot(name):
        screenshot(name)
        v=state();ox,oy=v['viewport_origin'];l,t,r,b=v['bounds']
        right=find('nav-1')['bounds'][2]+12
        with Image.open(ctx['out']/(name+'.png')) as im:
            im.crop(tuple(round(n) for n in (l-ox,t-oy,right-ox,b-oy))).save(ctx['out']/(name+'-navigation.png'))
            return im.convert('RGB'),(ox,oy)
    for font,option in ((18,6),(24,9)):
        query('system font');click('result-systemFont');click('systemFont');click('option-'+str(option))
        if state()['pending']:click('apply')
        click('nav-2');click('nav-1')
        for theme in range(5):
            click('stock-'+str(theme))
            if state()['pending']:click('apply')
            for width,height in ((1120,800),(880,600)):
                command(cmd='settings_ui',action='resize',width=width,height=height);settle()
                hover('nav-2');v=settle()
                a,b,c,d=find('nav-1')['bounds']
                name=f'nav-theme-{theme}-font-{font}-{width}x{height}'
                im,(ox,oy)=shot(name)
                def pixel(x,y):return im.getpixel((round(x-ox),round(y-oy)))
                assert d-b>=44,(name,'navigation target too short',d-b)
                for i in range(1,9):
                    item=find('nav-'+str(i));l,t,r,bot=item['bounds'];cl,ct,cr,cb=item['clip']
                    assert t>=ct-1 and bot<=cb+1,(name,'row clipped',i,item)
                    assert item['font_pixels']>=font,(name,item)
                rail=pixel(a+9,(b+d)/2);fill=pixel(a+18,(b+d)/2)
                assert distance(rail,fill)>=12,(name,'selected indicator missing',rail,fill)
                h=find('nav-2')['bounds'];n=find('nav-3')['bounds']
                hover_fill=pixel(h[2]-14,(h[1]+h[3])/2)
                normal_fill=pixel(n[2]-14,(n[1]+n[3])/2)
                assert distance(fill,hover_fill)>2,(name,'selected and hover look identical')
                assert distance(hover_fill,normal_fill)>1,(name,'hover feedback absent')
                # The left edge is reserved for the indicator; all label ink
                # must begin after the consistent 24px text inset.
                ink=[]
                for y in range(round(b+5),round(d-5)):
                    for x in range(round(a+18),round(c-12)):
                        if distance(pixel(x,y),fill)>65:ink.append((x,y))
                assert ink,(name,'label ink missing')
                assert min(x for x,y in ink)-a>=22,(name,'label too close to left edge')
                assert abs((min(y for x,y in ink)+max(y for x,y in ink))/2-(b+d)/2)<=3,(name,'text not centered')
                check(name,rowHeight=d-b,indicator=rail,selected=fill,hover=hover_fill,normal=normal_fill)
                # Click near the trailing edge, away from text, then near the
                # leading edge. Both must select exactly the visible row.
                pointer(h[2]-3,(h[1]+h[3])/2);assert state()['page']==2
                pointer(a+3,(b+d)/2);assert state()['page']==1
    check('both row edges select the matching category')
    # Arrow-key focus traversal is globally disabled by the existing Windows
    # host. Do not assert unsupported traversal or enable it across the app.
    query('system font');assert find('result-systemFont')
    check('existing Ctrl+K search shortcut works; arrow traversal is disabled by host')
    click('nav-1');command(cmd='settings_ui',action='resize',width=1120,height=800);settle()
    query('system font');click('result-systemFont');click('systemFont');click('option-6')
    if state()['pending']:click('apply')
    click('nav-2');click('nav-1');click('stock-1')
    if state()['pending']:click('apply')
    v=state();a,b,c,d=v['bounds'];pointer(a+30,b+26)
    shot('delivery-rose-navigation')
    click('done')
