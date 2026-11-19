// Original game memory is authoritative for online inventory balances.
#include "db_priv.h"

int db_account_online(Db *d, u32 mid, int *online, Err err) {
    Str s = {0}; MyResult r = {0};
    str_catf(&s,"SELECT COALESCE(MAX(login_status),0) FROM taiwan_login.login_account_3 WHERE m_id=%u",mid);
    int ok=select1(d,&s,&r);
    if(ok) *online=my_i64(&r,0,0,1)!=0;
    else snprintf(err,ERRBUF,"无法确认账号状态，未写入：%s",d->err);
    my_free(&r);str_free(&s);return ok;
}

static int online_submit(Db *d, const Charac *c, u32 amount, const u32 *extra, const char *operation, int version, OnlineCommand *request, Err err) {
    memset(request,0,sizeof(*request));
    request->version=version;
    Str s={0};MyResult r={0};u64 affected=0;
    str_cat(&s,"SELECT REPLACE(UUID(),'-','')");
    int ok=select1(d,&s,&r);
    if(ok && r.nrow==1) cp_text(request->token,sizeof(request->token),&r,0,0);
    my_free(&r);str_clear(&s);
    if(!ok || strlen(request->token)!=32) goto failed;
    str_catf(&s,"INSERT INTO d_taiwan.dnf_gm_online_v%d "
        "(token,m_id,charac_no,operation,amount%s,created,expires,updated) ",version,
        version==6?",amount_point":extra?",amount_sp2,amount_tp,amount_tp2":"");
    if(c->no) {
        str_catf(&s,"SELECT '%s',c.m_id,c.charac_no,'%s',%u",request->token,operation,amount);
        if(extra) str_catf(&s,",%u,%u,%u",extra[0],extra[1],extra[2]);
        str_catf(&s,",NOW(6),DATE_ADD(NOW(6),INTERVAL 20 SECOND),NOW(6) "
            "FROM taiwan_cain.charac_info c WHERE c.charac_no=%u AND c.m_id=%u AND c.delete_flag=0 ",c->no,c->mid);
    }
    else if(version==6) str_catf(&s,
        "SELECT '%s',c.UID,0,'refresh_billing',%u,%u,NOW(6),DATE_ADD(NOW(6),INTERVAL 20 SECOND),NOW(6) "
        "FROM d_taiwan.accounts c WHERE c.UID=%u ",request->token,amount,extra[0],c->mid);
    else str_catf(&s,
        "SELECT '%s',c.UID,0,'inspect',0,NOW(6),DATE_ADD(NOW(6),INTERVAL 20 SECOND),NOW(6) "
        "FROM d_taiwan.accounts c WHERE c.UID=%u ",request->token,c->mid);
    str_catf(&s,
        "AND EXISTS (SELECT 1 FROM taiwan_login.login_account_3 l JOIN d_taiwan.dnf_gm_hosts_v%d h "
        "ON h.channel_no=l.m_channel_no WHERE l.m_id=%u AND l.login_status>=1 "
        "AND h.seen>DATE_SUB(NOW(6),INTERVAL 5 SECOND))",
        version,c->mid);
    ok=run_aff(d,&s,&affected);
    str_free(&s);
    if(ok && affected==1) return 1;
    if(ok) snprintf(err,ERRBUF,"在线命令未提交：角色归属或状态变化，或游戏端桥接尚未就绪");
    else snprintf(err,ERRBUF,"在线命令未提交：%s；需要启用游戏端 GM 桥接",d->err);
    request->token[0]=0;return 0;
failed:
    str_free(&s);snprintf(err,ERRBUF,"创建在线命令失败：%s",d->err);request->token[0]=0;return 0;
}

int db_online_coin_submit(Db *d,const Charac *c,u32 amount,int add,OnlineCommand *request,Err err) {
    if(!c->no) {snprintf(err,ERRBUF,"在线奖励缺少角色编号");return 0;}
    return online_submit(d,c,amount,NULL,add?"add_coin":"set_coin",1,request,err);
}

int db_online_money_submit(Db *d,const Charac *c,u32 amount,int add,OnlineCommand *request,Err err) {
    if(!c->no) {snprintf(err,ERRBUF,"在线奖励缺少角色编号");return 0;}
    return online_submit(d,c,amount,NULL,add?"add_money":"set_money",2,request,err);
}

int db_online_character_submit(Db *d,u32 mid,OnlineCommand *request,Err err) {
    Charac c={0};c.mid=mid;
    int ok=online_submit(d,&c,0,NULL,"inspect",1,request,err);
    request->inspect=1;
    return ok;
}

int db_online_fatigue_submit(Db *d,const Charac *c,OnlineCommand *request,Err err) {
    if(!c->no) {snprintf(err,ERRBUF,"在线操作缺少角色编号");return 0;}
    return online_submit(d,c,0,NULL,"refill_fatigue",3,request,err);
}

int db_online_skill_submit(Db *d,const Charac *c,u32 sp,u32 sp2,u32 tp,u32 tp2,OnlineCommand *request,Err err) {
    if(!c->no) {snprintf(err,ERRBUF,"在线操作缺少角色编号");return 0;}
    u32 extra[]={sp2,tp,tp2};
    return online_submit(d,c,sp,extra,"set_skill_points",4,request,err);
}

int db_online_qp_submit(Db *d,const Charac *c,u32 qp,OnlineCommand *request,Err err) {
    if(!c->no) {snprintf(err,ERRBUF,"在线操作缺少角色编号");return 0;}
    return online_submit(d,c,qp,NULL,"set_quest_points",5,request,err);
}

int db_online_equipment_submit(Db *d,const Charac *c,u32 amount,int level,OnlineCommand *request,Err err) {
    if(!c->no) {snprintf(err,ERRBUF,"在线操作缺少角色编号");return 0;}
    return online_submit(d,c,amount,NULL,level?"unlock_equipment_level":"open_equipment_slots",7,request,err);
}

// Called inside the billing transaction, after its writes. Keep the reload
// request invisible to the game until those balances commit together.
int db_online_billing_submit(Db *d,u32 mid,OnlineCommand *request,Err err) {
    memset(request,0,sizeof(*request));
    int online=0;
    if(!db_account_online(d,mid,&online,err)) return 0;
    if(!online) return 1; // The next login loads the committed billing balance.
    Str s={0};MyResult r={0};
    str_catf(&s,"SELECT COALESCE(c.cera,0),COALESCE(p.cera_point,0) FROM d_taiwan.accounts a "
        "LEFT JOIN taiwan_billing.cash_cera c ON c.account=a.UID "
        "LEFT JOIN taiwan_billing.cash_cera_point p ON p.account=a.UID WHERE a.UID=%u",mid);
    int ok=select1(d,&s,&r);
    if(!ok) snprintf(err,ERRBUF,"读取点券余额：%s",d->err);
    else if(r.nrow!=1) {snprintf(err,ERRBUF,"点券账号不存在或账单记录不唯一");ok=0;}
    u64 cera=ok?my_u64(&r,0,0,UINT64_MAX):0,point=ok?my_u64(&r,0,1,UINT64_MAX):0;
    my_free(&r);str_free(&s);
    if(ok && (cera>UINT32_MAX || point>UINT32_MAX)) {snprintf(err,ERRBUF,"点券余额超出游戏范围");ok=0;}
    if(!ok) return 0;
    Charac c={0};c.mid=mid;u32 extra[]={(u32)point};
    return online_submit(d,&c,(u32)cera,extra,"refresh_billing",6,request,err);
}

int db_online_finish(Db *d, OnlineCommand *request, Err err) {
    int version=request->version==7?7:request->version==6?6:request->version==5?5:request->version==4?4:request->version==3?3:request->version==2?2:1;
    Str s={0};MyResult r={0};
    u64 deadline=now_ms()+22000;
    int stopped=0,ok=0;
    for(;;) {
        str_clear(&s);
        str_catf(&s,"SELECT state,note,expires<=NOW(6),charac_no FROM d_taiwan.dnf_gm_online_v%d WHERE token='%s'",version,request->token);
        if(!select1(d,&s,&r) || r.nrow!=1) {
            snprintf(err,ERRBUF,"在线命令 %s 结果不确定，请核对后再操作：%s",request->token,d->err);break;
        }
        const char *state=my_text(&r,0,0),*note=my_text(&r,0,1);
        if(state && !strcmp(state,"done")) {request->charac_no=(u32)my_u64(&r,0,3,0);ok=1;break;}
        if(state && (!strcmp(state,"rejected") || !strcmp(state,"unknown"))) {
            request->rejected=!strcmp(state,"rejected");
            // An inspect request has no requested character. This v1 bridge
            // reply means the account has no active game character (including
            // character select or a stale SQL online flag), not a failed write.
            // Mutation requests and ambiguous replies must still fail.
            if(request->inspect && request->rejected && note && !strcmp(note,"wrong_active_character")) {
                request->charac_no=0;ok=1;break;
            }
            snprintf(err,ERRBUF,"在线命令 %s %s（%s）；请核对，勿重复发放",request->token,
                !strcmp(state,"rejected")?"未执行":"结果不确定",note?note:"");break;
        }
        int expired=my_i64(&r,0,2,1)!=0;
        my_free(&r);
        if(expired || now_ms()>=deadline || db_cancelled()) {
            if(stopped) {
                snprintf(err,ERRBUF,"在线命令 %s 已被领取，结果不确定；请核对，勿重复发放",request->token);break;
            }
            str_clear(&s);
            str_catf(&s,"UPDATE d_taiwan.dnf_gm_online_v%d SET state='rejected',note='gm_cancelled',updated=NOW(6) "
                "WHERE token='%s' AND state='pending'",version,request->token);
            if(!run(d,&s)) {
                snprintf(err,ERRBUF,"在线命令 %s 取消确认失败，结果不确定：%s",request->token,d->err);break;
            }
            stopped=1;
            continue;
        }
        Sleep(100);
    }
    my_free(&r);str_free(&s);return ok;
}
