-- LuaSQL work is loaded inside the worker: the shipped luv crashes while
-- dumping this larger function on both original Linux and Windows hosts.
local database_work = function(encoded)
    local json = require('json')
    local cfg = json.decode(encoded)
    package.cpath = '/dp2/lua/?.so;' .. package.cpath
    local sql = require('luasql.mysql')
    local env, db
    local ok, result = pcall(function()
        env = assert(sql.mysql())
        db = assert(env:connect('d_taiwan', cfg.user, cfg.password, cfg.host, cfg.port))
        local function exec(text) return assert(db:execute(text)) end
        local function rows(text)
            local cursor = exec(text)
            local out, row = {}, cursor:fetch({}, 'a')
            while row do out[#out+1] = row; row = cursor:fetch({}, 'a') end
            cursor:close(); return out
        end
        local function token(value)
            assert(type(value)=='string' and #value==32 and value:match('^[0-9a-f]+$'), 'GM command token invalid')
            return "'" .. value .. "'"
        end
        -- Separate wire versions prevent older bridges from interpreting a new
        -- command as an existing operation during rollback or upgrade.
        local queues = {
            {version=1, suffix='v1', field='coin', operations="'set_coin','add_coin','inspect'"},
            {version=2, suffix='v2', field='money', operations="'set_money','add_money'"},
            {version=3, suffix='v3', field='fatigue', operations="'refill_fatigue'"},
            {version=4, suffix='v4', field='remain_sp', operations="'set_skill_points'"},
            {version=5, suffix='v5', field='qp', operations="'set_quest_points'"},
            {version=6, suffix='v6', operations="'refresh_billing'"},
            {version=7, suffix='v7', operations="'open_equipment_slots','unlock_equipment_level'"},
            {version=8, suffix='v8', field='inven_weight', operations="'raise_inventory_weight'"},
            {version=9, suffix='v9', operations="'set_character_level'"},
            {version=10, suffix='v10', field='pvp_grade', operations="'set_pvp_record'"},
            {version=11, suffix='v11', operations="'ready_quest'"},
        }
        if cfg.owner == '' then
            for _, queue in ipairs(queues) do
                local extra_columns = queue.version==3 and [[
                    before_used INT UNSIGNED NULL, before_premium INT UNSIGNED NULL,
                    after_premium INT UNSIGNED NULL,]] or queue.version==4 and [[
                    amount_sp2 INT UNSIGNED NOT NULL, amount_tp INT UNSIGNED NOT NULL, amount_tp2 INT UNSIGNED NOT NULL,
                    before_sp2 INT UNSIGNED NULL, after_sp2 INT UNSIGNED NULL,
                    before_tp INT UNSIGNED NULL, after_tp INT UNSIGNED NULL,
                    before_tp2 INT UNSIGNED NULL, after_tp2 INT UNSIGNED NULL,]] or queue.version==6 and [[
                    amount_point INT UNSIGNED NOT NULL,
                    before_point INT UNSIGNED NULL, after_point INT UNSIGNED NULL,]] or ''
                if queue.version==9 then extra_columns=[[
                    amount_exp INT UNSIGNED NOT NULL,
                    before_exp INT UNSIGNED NULL, after_exp INT UNSIGNED NULL,
                    after_sp INT UNSIGNED NULL, after_sp2 INT UNSIGNED NULL,
                    after_tp INT UNSIGNED NULL, after_tp2 INT UNSIGNED NULL,
                    after_money INT UNSIGNED NULL,]] end
                if queue.version==10 then extra_columns=[[
                    amount_win INT UNSIGNED NOT NULL, amount_point INT UNSIGNED NOT NULL,
                    before_win INT UNSIGNED NULL, after_win INT UNSIGNED NULL,
                    before_point INT UNSIGNED NULL, after_point INT UNSIGNED NULL,]] end
                if queue.version==11 then extra_columns='amount_slot INT UNSIGNED NOT NULL,' end
                exec(string.format([[CREATE TABLE IF NOT EXISTS dnf_gm_hosts_%s (
                    channel_no INT UNSIGNED PRIMARY KEY, owner CHAR(32) CHARACTER SET ascii NOT NULL,
                    seen DATETIME(6) NOT NULL) ENGINE=InnoDB]], queue.suffix))
                exec(string.format([[CREATE TABLE IF NOT EXISTS dnf_gm_online_%s (
                    token CHAR(32) CHARACTER SET ascii PRIMARY KEY,
                    m_id INT UNSIGNED NOT NULL, charac_no INT UNSIGNED NOT NULL,
                    operation ENUM(%s) NOT NULL, amount INT UNSIGNED NOT NULL,
                    state ENUM('pending','claimed','applied','done','rejected','unknown') NOT NULL DEFAULT 'pending',
                    owner CHAR(32) CHARACTER SET ascii NULL, before_value INT UNSIGNED NULL, after_value INT UNSIGNED NULL,
                    note VARCHAR(80) CHARACTER SET ascii NOT NULL DEFAULT '',
                    %s created DATETIME(6) NOT NULL, expires DATETIME(6) NOT NULL, updated DATETIME(6) NOT NULL,
                    INDEX pending_commands(state,created), INDEX account_commands(m_id,state,created)) ENGINE=InnoDB]],
                    queue.suffix, queue.operations, extra_columns))
            end
            return {owner=rows("SELECT REPLACE(UUID(),'-','') AS owner")[1].owner, commands={}}
        end
        local owner = token(cfg.owner)
        local commands = {}
        for _, queue in ipairs(queues) do
            local hosts, pending_table = 'dnf_gm_hosts_'..queue.suffix, 'dnf_gm_online_'..queue.suffix
            local other_work = ''
            for _, other in ipairs(queues) do
                if other.version~=queue.version then
                    other_work=other_work..string.format([[ AND NOT EXISTS (
                        SELECT 1 FROM dnf_gm_online_%s other WHERE other.m_id=q.m_id
                        AND other.state IN ('claimed','applied'))]],other.suffix)
                end
            end
            exec(string.format('INSERT IGNORE INTO %s VALUES(%d,%s,NOW(6))',hosts,cfg.channel,owner))
            exec(string.format([[UPDATE %s SET owner=%s,seen=NOW(6) WHERE channel_no=%d
                AND (owner=%s OR seen<DATE_SUB(NOW(6),INTERVAL 10 SECOND))]], hosts,owner,cfg.channel,owner))
            assert(rows(string.format('SELECT owner FROM %s WHERE channel_no=%d',hosts,cfg.channel))[1].owner==cfg.owner,
                   'another GM bridge owns this channel')
            for _, receipt in ipairs(cfg.receipts) do
                if (receipt.version or 1)==queue.version then
                    assert(receipt.state=='applied' or receipt.state=='rejected' or receipt.state=='unknown' or receipt.state=='done', 'GM receipt state invalid')
                    assert(receipt.note:match('^[a-z_]+$'), 'GM receipt note invalid')
                    local values = ''
                    if receipt.before then
                        values = string.format(',before_value=%u,after_value=%u', receipt.before, receipt.after)
                    end
                    if receipt.charac_no then values=values..string.format(',charac_no=%u',receipt.charac_no) end
                    if receipt.before_used then
                        values=values..string.format(',before_used=%u,before_premium=%u,after_premium=%u',
                            receipt.before_used,receipt.before_premium,receipt.after_premium)
                    end
                    if receipt.before_sp2 then
                        values=values..string.format(',before_sp2=%u,after_sp2=%u,before_tp=%u,after_tp=%u,before_tp2=%u,after_tp2=%u',
                            receipt.before_sp2,receipt.after_sp2,receipt.before_tp,receipt.after_tp,receipt.before_tp2,receipt.after_tp2)
                    end
                    if receipt.before_point then
                        values=values..string.format(',before_point=%u,after_point=%u',receipt.before_point,receipt.after_point)
                    end
                    if receipt.before_win then
                        values=values..string.format(',before_win=%u,after_win=%u',receipt.before_win,receipt.after_win)
                    end
                    if receipt.before_exp then
                        values=values..string.format(',before_exp=%u,after_exp=%u,after_sp=%u,after_sp2=%u,after_tp=%u,after_tp2=%u,after_money=%u',
                            receipt.before_exp,receipt.after_exp,receipt.after_sp,receipt.after_sp2,receipt.after_tp,receipt.after_tp2,receipt.after_money)
                    end
                    exec(string.format([[UPDATE %s SET state='%s',note='%s',updated=NOW(6)%s
                        WHERE token=%s AND owner=%s AND state='claimed']],pending_table,receipt.state,receipt.note,values,token(receipt.token),owner))
                end
            end
            -- Original save APIs enqueue work. Confirm SQL separately, without
            -- reporting enqueue as persistence or replaying the command.
            local saved_table = queue.version==3 and 'taiwan_cain.charac_stat' or 'taiwan_cain_2nd.inventory'
            local saved_extra = queue.version==3 and ' AND i.used_fatigue=0 AND i.premium_fatigue=q.after_premium' or ''
            if queue.version==4 then
                saved_table='taiwan_cain_2nd.skill'
                saved_extra=' AND i.remain_sp_2nd=q.after_sp2 AND i.remain_sfp_1st=q.after_tp AND i.remain_sfp_2nd=q.after_tp2'
            elseif queue.version==5 then
                saved_table='taiwan_cain.charac_quest_shop'
            elseif queue.version==8 then
                saved_table='taiwan_cain.charac_info'
            elseif queue.version==10 then
                saved_table='taiwan_cain.pvp_result'
                saved_extra=' AND i.win=q.after_win AND i.pvp_point=q.after_point'
            end
            if queue.version==9 then
                -- The original saves current experience in charac_stat;
                -- charac_info.exp is not refreshed by its normal save path.
                exec(string.format([[UPDATE %s q
                    JOIN taiwan_cain.charac_info c ON c.charac_no=q.charac_no
                    JOIN taiwan_cain.charac_stat s ON s.charac_no=q.charac_no
                    JOIN taiwan_cain_2nd.skill k ON k.charac_no=q.charac_no
                    JOIN taiwan_cain_2nd.inventory i ON i.charac_no=q.charac_no
                    SET q.state='done',q.note='saved',q.updated=NOW(6)
                    WHERE q.owner=%s AND q.state='applied' AND c.lev=q.after_value AND s.exp=q.after_exp
                    AND k.remain_sp=q.after_sp AND k.remain_sp_2nd=q.after_sp2
                    AND k.remain_sfp_1st=q.after_tp AND k.remain_sfp_2nd=q.after_tp2
                    AND i.money=q.after_money]],pending_table,owner))
            elseif queue.version==7 then
                exec(string.format([[UPDATE %s q
                    LEFT JOIN taiwan_cain.charac_stat s ON s.charac_no=q.charac_no
                    LEFT JOIN taiwan_cain.charac_manage_info m ON m.charac_no=q.charac_no
                    SET q.state='done',q.note='saved',q.updated=NOW(6)
                    WHERE q.owner=%s AND q.state='applied' AND
                    ((q.operation='open_equipment_slots' AND s.add_slot_flag=q.after_value) OR
                     (q.operation='unlock_equipment_level' AND m.max_equip_level=q.after_value))]],pending_table,owner))
            elseif queue.version==11 then
                local ids,triggers='CASE q.amount_slot','CASE q.amount_slot'
                for slot=1,20 do
                    ids=ids..string.format(' WHEN %d THEN i.play_%d',slot,slot)
                    triggers=triggers..string.format(' WHEN %d THEN i.play_%d_trigger',slot,slot)
                end
                exec(string.format([[UPDATE %s q JOIN taiwan_cain.new_charac_quest i ON i.charac_no=q.charac_no
                    SET q.state='done',q.note='saved',q.updated=NOW(6)
                    WHERE q.owner=%s AND q.state='applied' AND (%s END)=q.amount AND (%s END)=0]],
                    pending_table,owner,ids,triggers))
            elseif queue.version~=6 then
                exec(string.format([[UPDATE %s q JOIN %s i ON i.charac_no=q.charac_no
                    SET q.state='done',q.note='saved',q.updated=NOW(6)
                    WHERE q.owner=%s AND q.state='applied' AND i.%s=q.after_value%s]],pending_table,saved_table,owner,queue.field,saved_extra))
            end
            if queue.version~=6 then
                exec(string.format([[UPDATE %s SET state='unknown',note='save_unconfirmed',updated=NOW(6)
                    WHERE owner=%s AND state='applied' AND updated<DATE_SUB(NOW(6),INTERVAL 15 SECOND)]],pending_table,owner))
            end
            -- Lost work and a previous host's commands are ambiguous, never pending
            -- again. End them explicitly so they cannot block this account forever.
            exec(string.format([[UPDATE %s q SET state='unknown',note='host_or_work_lost',updated=NOW(6)
                WHERE q.state IN ('claimed','applied') AND q.expires<NOW(6)
                AND NOT EXISTS (SELECT 1 FROM %s h WHERE h.owner=q.owner
                    AND h.seen>DATE_SUB(NOW(6),INTERVAL 10 SECOND))]],pending_table,hosts))
            exec(string.format([[UPDATE %s SET state='unknown',note='work_unconfirmed',updated=NOW(6)
                WHERE owner=%s AND state='claimed' AND expires<NOW(6)]],pending_table,owner))
            exec(string.format([[UPDATE %s SET state='rejected',note='expired',updated=NOW(6)
                WHERE state='pending' AND expires<NOW(6)]],pending_table))
            local extra_fields=queue.version==4 and ',q.amount_sp2,q.amount_tp,q.amount_tp2' or
                queue.version==6 and ',q.amount_point' or queue.version==9 and ',q.amount_exp' or
                queue.version==10 and ',q.amount_win,q.amount_point' or queue.version==11 and ',q.amount_slot' or ''
            local pending = rows(string.format([[SELECT q.token,q.m_id,q.charac_no,q.operation,q.amount%s
                FROM %s q JOIN taiwan_login.login_account_3 l ON l.m_id=q.m_id
                WHERE q.state='pending' AND q.expires>NOW(6) AND l.login_status>=1 AND l.m_channel_no=%d
                AND NOT EXISTS (SELECT 1 FROM %s prior WHERE prior.m_id=q.m_id
                    AND (prior.state IN ('claimed','applied') OR (prior.state='pending'
                        AND (prior.created<q.created OR (prior.created=q.created AND prior.token<q.token)))))
                %s ORDER BY q.created LIMIT 32]],extra_fields,pending_table,cfg.channel,pending_table,other_work))
            for _, req in ipairs(pending) do
                local balance_values=''
                if queue.version==6 then
                    -- A reload follows all committed billing changes, including
                    -- concurrent awards. Its acknowledgement is not a replay of
                    -- the amount originally awarded by any one GM transaction.
                    local balance=rows(string.format([[SELECT COALESCE(c.cera,0) AS cera,COALESCE(p.cera_point,0) AS point
                        FROM d_taiwan.accounts a LEFT JOIN taiwan_billing.cash_cera c ON c.account=a.UID
                        LEFT JOIN taiwan_billing.cash_cera_point p ON p.account=a.UID WHERE a.UID=%u]],tonumber(req.m_id)))[1]
                    assert(balance,'billing account disappeared')
                    req.amount,req.amount_point=balance.cera,balance.point
                    balance_values=string.format(',amount=%u,amount_point=%u',tonumber(req.amount),tonumber(req.amount_point))
                end
                local changed = exec(string.format([[UPDATE %s SET state='claimed',owner=%s,updated=NOW(6)%s
                    WHERE token=%s AND state='pending' AND expires>NOW(6)]],pending_table,owner,balance_values,token(req.token)))
                if changed == 1 then req.version=queue.version; commands[#commands+1] = req end
            end
        end
        return {owner=cfg.owner, commands=commands}
    end)
    if db then db:close() end
    if env then env:close() end
    return json.encode({ok=ok,result=result})
end
if ...=='worker' then return database_work end

-- Online character commands for the pinned original game and its DP2 API.
-- Database I/O runs in luv workers. Only the completion callback calls game APIs.
local previous = rawget(_G, 'DNF_GM_ONLINE')
if previous then return previous end
local uv, json = require('luv'), require('json')
local ffi, game = require('ffi'), require('df.game')
local world, symbols = require('df.game.mgr.world'), require('df.game.symbols.core')
local input = assert(io.open('/dp2/gm-online.private.json', 'rb'), 'GM bridge configuration missing')
local config = json.decode(input:read('*a')); input:close()
assert(type(config.host) == 'string' and type(config.user) == 'string' and type(config.password) == 'string', 'GM database configuration invalid')
assert(math.type(config.port) == 'integer' and config.port > 0 and config.port <= 65535, 'GM database port invalid')
assert(math.type(config.channel) == 'integer' and config.channel > 0, 'GM channel invalid')
ffi.cdef[[
unsigned int DNF_GM_GetCoin(CInventory *);
void DNF_GM_SetCoin(CInventory *, unsigned int);
int DNF_GM_SaveMoneyCoin(CUser *);
void DNF_GM_SendCoin(CUser *);
unsigned int DNF_GM_GetMoney(CInventory *);
void DNF_GM_SetMoney(CInventory *, unsigned int);
int DNF_GM_MoneyLimit(CInventory *, int);
unsigned short DNF_GM_Fatigue(CUser *);
unsigned short DNF_GM_MaxFatigue(CUser *);
unsigned short DNF_GM_PremiumFatigue(CUser *);
unsigned short DNF_GM_MaxPremiumFatigue(CUser *);
unsigned short DNF_GM_UsedFatigue(CUser *);
void DNF_GM_SetFatigue(CUser *, int);
void DNF_GM_SetPremiumFatigue(CUser *, unsigned short);
void DNF_GM_ResetUsedFatigue(CUser *);
void DNF_GM_SendFatigue(CUser *);
void DNF_GM_SaveCharac(CUser *);
void *DNF_GM_SkillRead(CUser *);
void *DNF_GM_SkillWrite(CUser *);
void DNF_GM_GetSkillPoints(void *, int *, int *, int *, int *);
void DNF_GM_SetSkillPoints(void *, int, int, int, int);
void DNF_GM_SendSkill(CUser *);
void DNF_GM_UpdateData(CUser *);
int DNF_GM_GetQP(CUser *);
void DNF_GM_SetQP(CUser *, int);
void *DNF_GM_GetExpandData(CUser *, int);
extern void *DNF_GM_IPGHelper;
void DNF_GM_QueryCash(void *, CUser *);
unsigned int DNF_GM_GetCera(CUser *);
unsigned int DNF_GM_GetCeraPoint(CUser *);
void DNF_GM_SendCash(CUser *, bool);
unsigned char DNF_GM_GetEquipmentSlots(CUser *);
void DNF_GM_SetEquipmentSlots(CUser *, char);
void DNF_GM_SaveCharacStat(CUser *);
int DNF_GM_GetEquipmentLevel(CUser *);
void DNF_GM_SetEquipmentLevel(CUser *, int);
void DNF_GM_SendEquipmentLevel(CUser *);
int DNF_GM_SendBasicInfo(CUser *, int, int, int);
void *DNF_GM_PropertiesRead(CUser *);
void *DNF_GM_PropertiesWrite(CUser *);
void *DNF_GM_DataManager();
int DNF_GM_LevelExperience(void *, int);
int DNF_GM_LevelSkillPoints(void *, int);
int DNF_GM_GetLevel(CUser *);
int DNF_GM_GetExperience(CUser *);
void DNF_GM_SetExperience(CUser *, int);
int DNF_GM_SetLevel(CUser *, int);
void DNF_GM_SetStoredLevel(CUser *, int);
void DNF_GM_OnLevelUp(CUser *);
void DNF_GM_RefreshMaxLevel(CUser *);
const int *DNF_GM_PvpRead(CUser *);
int *DNF_GM_PvpWrite(CUser *);
void DNF_GM_SendPvp(CUser *);
const char *DNF_GM_QuestRead(CUser *);
bool DNF_GM_QuestCondition(const char *,int);
bool DNF_GM_GetQuestFlag(CUser *);
void DNF_GM_SetQuestFlag(CUser *,bool);
void DNF_GM_QuestAction(CUser *,int,int,int,int);
]]
symbols.alias('DNF_GM_GetCoin', '_ZNK10CInventory7GetCoinEv')
symbols.alias('DNF_GM_SetCoin', '_ZN10CInventory7SetCoinEj')
symbols.alias('DNF_GM_SaveMoneyCoin', '_ZN5CUser13SaveMoneyCoinEv')
symbols.alias('DNF_GM_SendCoin', '_ZN5CUser8SendCoinEv')
symbols.alias('DNF_GM_GetMoney', '_ZNK10CInventory9get_moneyEv')
symbols.alias('DNF_GM_SetMoney', '_ZN10CInventory9set_moneyEj')
symbols.alias('DNF_GM_MoneyLimit', '_ZN10CInventory13getValidMoneyEi')
symbols.alias('DNF_GM_Fatigue', '_ZNK15CUserCharacInfo19getCurCharacFatigueEv')
symbols.alias('DNF_GM_MaxFatigue', '_ZNK15CUserCharacInfo22getCurCharacMaxFatigueEv')
symbols.alias('DNF_GM_PremiumFatigue', '_ZNK15CUserCharacInfo26getCurCharacPremiumFatigueEv')
symbols.alias('DNF_GM_MaxPremiumFatigue', '_ZNK15CUserCharacInfo29getCurCharacMaxPremiumFatigueEv')
symbols.alias('DNF_GM_UsedFatigue', '_ZN15CUserCharacInfo23getCurCharacUsedFatigueEv')
symbols.alias('DNF_GM_SetFatigue', '_ZN15CUserCharacInfo19setCurCharacFatigueEi')
symbols.alias('DNF_GM_SetPremiumFatigue', '_ZN15CUserCharacInfo26setCurCharacPremiumFatigueEt')
symbols.alias('DNF_GM_ResetUsedFatigue', '_ZN15CUserCharacInfo25resetCurCharacUsedFatigueEv')
symbols.alias('DNF_GM_SendFatigue', '_ZN5CUser11SendFatigueEv')
symbols.alias('DNF_GM_SaveCharac', '_ZN5CUser15_SaveCharacDataEv')
symbols.alias('DNF_GM_SkillRead', '_ZNK15CUserCharacInfo18getCurCharacSkillREv')
symbols.alias('DNF_GM_SkillWrite', '_ZN15CUserCharacInfo18getCurCharacSkillWEv')
symbols.alias('DNF_GM_GetSkillPoints', '_ZNK9SkillSlot13get_remain_spEPiS0_S0_S0_')
symbols.alias('DNF_GM_SetSkillPoints', '_ZN9SkillSlot13set_remain_spEiiii')
symbols.alias('DNF_GM_SendSkill', '_ZN5CUser15send_skill_infoEv')
symbols.alias('DNF_GM_UpdateData', '_ZN5CUser10UpdateDataEv')
symbols.alias('DNF_GM_GetQP', '_ZN5CUser13getQuestPointEv')
symbols.alias('DNF_GM_SetQP', '_ZN5CUser13setQuestPointEi')
symbols.alias('DNF_GM_GetExpandData', '_ZNK5CUser19GetCharacExpandDataE23ENUM_CHARAC_EXPAND_TYPE')
symbols.alias('DNF_GM_IPGHelper','_ZN10GlobalData12s_pIPGHelperE')
symbols.alias('DNF_GM_QueryCash','_ZN8WongWork3IPG10CIPGHelper8IPGQueryEP5CUser')
symbols.alias('DNF_GM_GetCera','_ZN5CUser7GetCeraEv')
symbols.alias('DNF_GM_GetCeraPoint','_ZN5CUser12GetCeraPointEv')
symbols.alias('DNF_GM_SendCash','_ZN5CUser12SendCashDataEb')
symbols.alias('DNF_GM_GetEquipmentSlots','_ZN15CUserCharacInfo21GetCurExEquipSlotStatEv')
symbols.alias('DNF_GM_SetEquipmentSlots','_ZN15CUserCharacInfo22SetCurExpEquipSlotStatEc')
symbols.alias('DNF_GM_SaveCharacStat','_ZN15CUserCharacInfo20enableSaveCharacStatEv')
symbols.alias('DNF_GM_GetEquipmentLevel','_ZNK15CUserCharacInfo25GetCurCharacMaxEquipLevelEv')
symbols.alias('DNF_GM_SetEquipmentLevel','_ZN15CUserCharacInfo25SetCurCharacMaxEquipLevelEi')
symbols.alias('DNF_GM_SendEquipmentLevel','_ZN5CUser17SendMaxEquipLevelEv')
-- Original eSendTarget 1 is self; 0 broadcasts a visible user's packet to online players.
symbols.alias('DNF_GM_SendBasicInfo','_ZN5CUser14SendNotiPacketENS_11eSendTargetE15ENUM_NOTIPACKETi')
symbols.alias('DNF_GM_PropertiesRead','_ZNK15CUserCharacInfo20getCurCharacAddInfoREv')
symbols.alias('DNF_GM_PropertiesWrite','_ZN15CUserCharacInfo20getCurCharacAddInfoWEv')
symbols.alias('DNF_GM_DataManager','_Z14G_CDataManagerv')
symbols.alias('DNF_GM_LevelExperience','_ZNK12CDataManager13get_level_expEi')
symbols.alias('DNF_GM_LevelSkillPoints','_ZNK12CDataManager14GetSpAtLevelUpEi')
symbols.alias('DNF_GM_GetLevel','_ZNK15CUserCharacInfo16get_charac_levelEv')
symbols.alias('DNF_GM_GetExperience','_ZNK15CUserCharacInfo15getCurCharacExpEv')
symbols.alias('DNF_GM_SetExperience','_ZN15CUserCharacInfo15setCurCharacExpEi')
symbols.alias('DNF_GM_SetLevel','_ZN5CUser14SetCharacLevelEi')
symbols.alias('DNF_GM_SetStoredLevel','_ZN15CUserCharacInfo17setCurCharacLevelEi')
symbols.alias('DNF_GM_OnLevelUp','_ZN5CUser10_onLevelUpEv')
symbols.alias('DNF_GM_RefreshMaxLevel','_ZN5CUser21CalcurateUserMaxLevelEv')
symbols.alias('DNF_GM_PvpRead','_ZNK15CUserCharacInfo13getPVPResultREv')
symbols.alias('DNF_GM_PvpWrite','_ZN15CUserCharacInfo13getPVPResultWEv')
symbols.alias('DNF_GM_SendPvp','_ZN5CUser15send_pvp_recordEv')
symbols.alias('DNF_GM_QuestRead','_ZNK5CUser18getCurCharacQuestREv')
symbols.alias('DNF_GM_QuestCondition','_ZN9UserQuest21check_quest_conditionEi')
symbols.alias('DNF_GM_GetQuestFlag','_ZN5CUser14getGmQuestFlagEv')
symbols.alias('DNF_GM_SetQuestFlag','_ZN5CUser14setGmQuestFlagEb')
symbols.alias('DNF_GM_QuestAction','_ZN5CUser12quest_actionEiiii')

local function inventory_weight(ptr,write)
    local data=write and ffi.C.DNF_GM_PropertiesWrite(ptr) or ffi.C.DNF_GM_PropertiesRead(ptr)
    assert(data~=nil and tonumber(ffi.cast('unsigned int',data))~=0,'character properties missing')
    -- Pinned original loader/setter: inven_weight is at character +0xc2;
    -- the original properties accessor returns character +0x88, packed.
    return ffi.cast('int *',ffi.cast('char *',data)+0x3a)
end

local function skill_points(ptr)
    local values=ffi.new('int[4]')
    ffi.C.DNF_GM_GetSkillPoints(ffi.C.DNF_GM_SkillRead(ptr),values,values+1,values+2,values+3)
    return {tonumber(values[0]),tonumber(values[1]),tonumber(values[2]),tonumber(values[3])}
end

-- Preflight the original per-level point changes before its setters mutate
-- anything: the original storage is int32, but the skill notification is uint16.
local function level_skill_points(ptr,before,after,manager)
    local points=skill_points(ptr)
    local function valid()
        for _,value in ipairs(points) do if value<0 or value>65535 then return false end end
        return true
    end
    if not valid() then return nil end
    local step=after>=before and 1 or -1
    local old=before
    for target=before+step,after,step do
        local delta=step*tonumber(ffi.C.DNF_GM_LevelSkillPoints(manager,step==1 and target or target+1))
        for i=1,2 do points[i]=math.max(0,math.min(1073741823,points[i]+delta)) end
        for i=3,4 do points[i]=target>49 and (old>50 and points[i]+step or target-49) or 0 end
        if not valid() then return nil end
        old=target
    end
    return points
end

local state = {owner='', receipts={}, busy=false, billing={}}
local worker = uv.new_work(function(encoded)
    package.path='/dp2/lua/?.lua;/dp2/lua/?/init.lua;'..package.path
    return assert(loadfile('/dp2/dnf-gm-online.lua'))('worker')(encoded)
end,function(encoded)
    state.busy = false
    if state.stopping then return end
    local decoded = json.decode(encoded)
    if not decoded.ok then
        -- Keep unacknowledged receipts. A later worker can repeat only the SQL
        -- receipt update, never the original game operation.
        if state.error ~= decoded.result then io.stderr:write('GM bridge: ',tostring(decoded.result),'\n') end
        state.error = decoded.result
        return
    end
    state.error = nil
    state.owner = decoded.result.owner
    state.receipts = {}
    for _, req in ipairs(decoded.result.commands) do
        local receipt = {token=req.token,version=req.version,state='rejected',note='wrong_active_character'}
        local ptr = world.FindUserByAcc(tonumber(req.m_id))
        local user = ptr and game.fac.user(ptr)
        local money = req.version==2 and (req.operation=='set_money' or req.operation=='add_money')
        local coin = req.version==1 and (req.operation=='set_coin' or req.operation=='add_coin')
        local fatigue = req.version==3 and req.operation=='refill_fatigue'
        local skills = req.version==4 and req.operation=='set_skill_points'
        local quest_points = req.version==5 and req.operation=='set_quest_points'
        local billing = req.version==6 and req.operation=='refresh_billing'
        local equipment_slots = req.version==7 and req.operation=='open_equipment_slots'
        local equipment_level = req.version==7 and req.operation=='unlock_equipment_level'
        local weight = req.version==8 and req.operation=='raise_inventory_weight'
        local level = req.version==9 and req.operation=='set_character_level'
        local pvp = req.version==10 and req.operation=='set_pvp_record'
        local ready_quest = req.version==11 and req.operation=='ready_quest'
        local waiting=false
        if req.operation=='inspect' and req.version==1 then
            if user and user:GetState()>=3 and user:GetCharacNo()>0 then
                receipt.state,receipt.note,receipt.charac_no='done','active_character',user:GetCharacNo()
            end
        elseif billing then
            -- The SQL billing transaction is already committed. Only request its
            -- original asynchronous reload; never write a cached balance to SQL
            -- or increment a reward again when refreshing fails.
            receipt.state,receipt.note='unknown','billing_user_unavailable'
            if user and user:GetState()>=3 and ffi.C.DNF_GM_IPGHelper~=nil then
                receipt.before=tonumber(ffi.C.DNF_GM_GetCera(user.fptr))
                receipt.before_point=tonumber(ffi.C.DNF_GM_GetCeraPoint(user.fptr))
                receipt.after,receipt.after_point=tonumber(req.amount),tonumber(req.amount_point)
                local ok=pcall(ffi.C.DNF_GM_QueryCash,ffi.C.DNF_GM_IPGHelper,user.fptr)
                if ok then
                    state.billing[#state.billing+1]={mid=tonumber(req.m_id),receipt=receipt,deadline=uv.now()+15000}
                    waiting=true
                else receipt.note='billing_query_failed' end
            end
        elseif skills and user and user:GetState()>=3 and user:GetCharacNo()==tonumber(req.charac_no) then
            local after={tonumber(req.amount),tonumber(req.amount_sp2),tonumber(req.amount_tp),tonumber(req.amount_tp2)}
            local before=skill_points(user.fptr)
            local valid=true
            for i=1,4 do
                if not after[i] or after[i]<0 or after[i]>65535 or after[i]%1~=0 or before[i]<0 then valid=false end
            end
            if valid then
                receipt.before,receipt.after=before[1],after[1]
                receipt.before_sp2,receipt.after_sp2=before[2],after[2]
                receipt.before_tp,receipt.after_tp=before[3],after[3]
                receipt.before_tp2,receipt.after_tp2=before[4],after[4]
                receipt.state,receipt.note='unknown','apply_failed'
                local ok=pcall(function()
                    ffi.C.DNF_GM_SetSkillPoints(ffi.C.DNF_GM_SkillWrite(user.fptr),table.unpack(after))
                    local actual=skill_points(user.fptr)
                    for i=1,4 do assert(actual[i]==after[i],'skill points readback differs') end
                    ffi.C.DNF_GM_SendSkill(user.fptr)
                    -- _SaveCharacData does not serialize skills; use the original
                    -- dirty-skill path, which also retains learned skills/commands.
                    ffi.C.DNF_GM_UpdateData(user.fptr)
                end)
                if ok then receipt.state,receipt.note='applied','save_queued' end
            else receipt.note='skill_points_limit' end
        elseif quest_points and user and user:GetState()>=3 and user:GetCharacNo()==tonumber(req.charac_no) then
            local after=tonumber(req.amount)
            -- The original QP dispatchers use expand type 11. The public getter
            -- returns zero when absent, so check the module before accepting zero.
            if ffi.C.DNF_GM_GetExpandData(user.fptr,11)==nil then
                receipt.note='quest_shop_unavailable'
            else
                local before=tonumber(ffi.C.DNF_GM_GetQP(user.fptr))
                if after and after>=0 and after<=2147483647 and after%1==0 and before>=0 then
                    receipt.before,receipt.after=before,after
                    receipt.state,receipt.note='unknown','apply_failed'
                    local ok=pcall(function()
                        -- setQuestPoint marks the original expand data dirty and
                        -- sends the 32-bit notification. UpdateData saves it.
                        ffi.C.DNF_GM_SetQP(user.fptr,after)
                        assert(tonumber(ffi.C.DNF_GM_GetQP(user.fptr))==after,'QP readback differs')
                        ffi.C.DNF_GM_UpdateData(user.fptr)
                    end)
                    if ok then receipt.state,receipt.note='applied','save_queued' end
                else receipt.note='quest_points_limit' end
            end
        elseif ready_quest and user and user:GetState()==3 and user:GetCharacNo()==tonumber(req.charac_no) then
            local id,slot=tonumber(req.amount),tonumber(req.amount_slot)
            if id>=1 and id<30000 and slot>=1 and slot<=20 then
                local data=ffi.C.DNF_GM_QuestRead(user.fptr)
                -- Original quest loader/save use 20 IDs and 20 packed counters.
                local ids=ffi.cast('const unsigned int *',data+0x7538)
                local triggers=ffi.cast('const unsigned int *',data+0x7588)
                local matches=0
                for n=0,19 do if tonumber(ids[n])==id then matches=matches+1 end end
                if tonumber(ids[slot-1])~=id or matches~=1 then receipt.note='quest_slot_changed'
                elseif not ffi.C.DNF_GM_QuestCondition(data,id) then receipt.note='quest_condition'
                else
                    receipt.before,receipt.after=tonumber(triggers[slot-1]),0
                    receipt.state,receipt.note='unknown','quest_update_incomplete'
                    local flag=ffi.C.DNF_GM_GetQuestFlag(user.fptr)
                    -- No yield while the original GM flag is set. The original
                    -- action updates counters, sends command 35 and APC notice.
                    -- Preflight above avoids its clear-before-condition failure.
                    ffi.C.DNF_GM_SetQuestFlag(user.fptr,true)
                    local ok=pcall(function()ffi.C.DNF_GM_QuestAction(user.fptr,35,id,0,0)end)
                    ffi.C.DNF_GM_SetQuestFlag(user.fptr,flag)
                    if ok then ok=pcall(function()
                        assert(ffi.C.DNF_GM_GetQuestFlag(user.fptr)==flag,'quest GM flag not restored')
                        assert(tonumber(ids[slot-1])==id and tonumber(triggers[slot-1])==0,'quest readback differs')
                        ffi.C.DNF_GM_UpdateData(user.fptr)
                    end)end
                    if ok then receipt.state,receipt.note='applied','save_queued' end
                end
            else receipt.note='quest_limit' end
        elseif pvp and user and user:GetState()==3 and user:GetCharacNo()==tonumber(req.charac_no) then
            local grade,win,point=tonumber(req.amount),tonumber(req.amount_win),tonumber(req.amount_point)
            if grade and grade>=0 and grade<=37 and grade%1==0 and
                win and win>=0 and win<=2147483647 and win%1==0 and
                point and point>=0 and point<=2147483647 and point%1==0 then
                local data=ffi.C.DNF_GM_PvpRead(user.fptr)
                -- Fixed original loader and GM rank setter: win at +0,
                -- pvp_point at +8, pvp_grade at +20; keep all other fields.
                local before_grade,before_win,before_point=tonumber(data[5]),tonumber(data[0]),tonumber(data[2])
                if before_grade>=0 and before_win>=0 and before_point>=0 then
                    receipt.before,receipt.after=before_grade,grade
                    receipt.before_win,receipt.after_win=before_win,win
                    receipt.before_point,receipt.after_point=before_point,point
                    receipt.state,receipt.note='unknown','pvp_update_incomplete'
                    local ok=pcall(function()
                        -- Reacquiring the writable accessor also restores the
                        -- dirty flag when retrying a previously failed save.
                        local writable=ffi.C.DNF_GM_PvpWrite(user.fptr)
                        writable[5],writable[0],writable[2]=grade,win,point
                        assert(ffi.C.DNF_GM_SendBasicInfo(user.fptr,0,2,0)==1,'PVP public notification failed')
                        ffi.C.DNF_GM_SendPvp(user.fptr)
                        local after=ffi.C.DNF_GM_PvpRead(user.fptr)
                        assert(tonumber(after[5])==grade and tonumber(after[0])==win and tonumber(after[2])==point,'PVP readback differs')
                        ffi.C.DNF_GM_UpdateData(user.fptr)
                    end)
                    if ok then receipt.state,receipt.note='applied','save_queued' end
                else receipt.note='pvp_current_invalid' end
            else receipt.note='pvp_limit' end
        elseif level and user and user:GetState()==3 and user:GetCharacNo()==tonumber(req.charac_no) then
            local before,after=tonumber(ffi.C.DNF_GM_GetLevel(user.fptr)),tonumber(req.amount)
            local before_exp,target_exp=tonumber(ffi.C.DNF_GM_GetExperience(user.fptr)),tonumber(req.amount_exp)
            local manager=ffi.C.DNF_GM_DataManager()
            if after>=1 and after<=127 and before>=1 and before<=127 and target_exp>=0 and target_exp<=2147483647
                and tonumber(ffi.C.DNF_GM_LevelExperience(manager,after))==target_exp
                and (after==1 or target_exp>tonumber(ffi.C.DNF_GM_LevelExperience(manager,after-1))) then
                local projected=level_skill_points(user.fptr,before,after,manager)
                if not projected then receipt.note='level_skill_points_limit'
                else
                receipt.before,receipt.after=before,after
                receipt.state,receipt.note='unknown','apply_failed'
                local ok=pcall(function()
                    -- Original GM level changes advance one level at a time,
                    -- preserving its per-level SP/TP, stat and reward rules.
                    local inventory=assert(user:GetCurCharacInvenR(),'inventory missing')
                    local step=after>=before and 1 or -1
                    for next_level=before+step,after,step do
                        assert(ffi.C.DNF_GM_SetLevel(user.fptr,next_level)==1,'level setter failed')
                    end
                    -- Failed original saves clear their dirty flags. A same-level
                    -- retry requeues current data without repeating points/rewards.
                    if before==after then
                        ffi.C.DNF_GM_SetStoredLevel(user.fptr,after)
                        ffi.C.DNF_GM_SkillWrite(user.fptr)
                    end
                    if before~=after then ffi.C.DNF_GM_OnLevelUp(user.fptr) end
                    ffi.C.DNF_GM_SetExperience(user.fptr,target_exp)
                    ffi.C.DNF_GM_RefreshMaxLevel(user.fptr)
                    assert(ffi.C.DNF_GM_SendBasicInfo(user.fptr,1,37,0)==1,'level notification failed')
                    assert(ffi.C.DNF_GM_SendBasicInfo(user.fptr,0,2,0)==1,'public level notification failed')
                    assert(ffi.C.DNF_GM_SendBasicInfo(user.fptr,1,2,1)==1,'level properties notification failed')
                    ffi.C.DNF_GM_SendSkill(user.fptr)
                    assert(tonumber(ffi.C.DNF_GM_GetLevel(user.fptr))==after and
                        tonumber(ffi.C.DNF_GM_GetExperience(user.fptr))==target_exp,'level readback differs')
                    local sp=skill_points(user.fptr)
                    for i=1,4 do assert(sp[i]==projected[i],'level skill points differ') end
                    receipt.before_exp,receipt.after_exp=before_exp,target_exp
                    receipt.after_sp,receipt.after_sp2,receipt.after_tp,receipt.after_tp2=sp[1],sp[2],sp[3],sp[4]
                    receipt.after_money=tonumber(ffi.C.DNF_GM_GetMoney(inventory.fptr))
                    ffi.C.DNF_GM_UpdateData(user.fptr)
                end)
                if ok then receipt.state,receipt.note='applied','save_queued' end
                end
            else receipt.note='level_curve_mismatch' end
        elseif level and user and user:GetCharacNo()==tonumber(req.charac_no) then
            receipt.note='level_requires_town'
        elseif weight and user and user:GetState()>=3 and user:GetCharacNo()==tonumber(req.charac_no) then
            local before,after=tonumber(inventory_weight(user.fptr,false)[0]),tonumber(req.amount)
            if before>=0 and after==99999999 then
                receipt.before,receipt.after=before,after
                receipt.state,receipt.note='unknown','apply_failed'
                local ok=pcall(function()
                    -- The mutable accessor marks character properties dirty.
                    inventory_weight(user.fptr,true)[0]=after
                    assert(ffi.C.DNF_GM_SendBasicInfo(user.fptr,1,2,1)==1,'weight notification failed')
                    assert(tonumber(inventory_weight(user.fptr,false)[0])==after,'weight readback differs')
                    ffi.C.DNF_GM_SaveCharac(user.fptr)
                end)
                if ok then receipt.state,receipt.note='applied','save_queued' end
            else receipt.note='weight_limit' end
        elseif (equipment_slots or equipment_level) and user and user:GetState()>=3 and user:GetCharacNo()==tonumber(req.charac_no) then
            local get=equipment_slots and ffi.C.DNF_GM_GetEquipmentSlots or ffi.C.DNF_GM_GetEquipmentLevel
            local before,after=tonumber(get(user.fptr)),tonumber(req.amount)
            if after and after%1==0 and ((equipment_slots and after==3 and before<=3) or
                (equipment_level and after>=1 and after<=255 and before>=0)) then
                receipt.before,receipt.after=before,after
                receipt.state,receipt.note='unknown','apply_failed'
                local ok=pcall(function()
                    if equipment_slots then
                        ffi.C.DNF_GM_SetEquipmentSlots(user.fptr,after)
                        ffi.C.DNF_GM_SaveCharacStat(user.fptr)
                        assert(ffi.C.DNF_GM_SendBasicInfo(user.fptr,1,2,1)==1,'equipment slot notification failed')
                    else
                        -- The original setter marks management data dirty and
                        -- preserves the tag character and striker selection.
                        ffi.C.DNF_GM_SetEquipmentLevel(user.fptr,after)
                        ffi.C.DNF_GM_SendEquipmentLevel(user.fptr)
                    end
                    assert(tonumber(get(user.fptr))==after,'equipment readback differs')
                    ffi.C.DNF_GM_SaveCharac(user.fptr)
                end)
                if ok then receipt.state,receipt.note='applied','save_queued' end
            else receipt.note='equipment_limit' end
        elseif fatigue and user and user:GetState()>=3 and user:GetCharacNo()==tonumber(req.charac_no) then
            local ptr=user.fptr
            local maximum=tonumber(ffi.C.DNF_GM_MaxFatigue(ptr))
            local premium=tonumber(ffi.C.DNF_GM_MaxPremiumFatigue(ptr))
            if maximum>0 and maximum<=32767 and premium<=32767 then
                receipt.before,receipt.after=tonumber(ffi.C.DNF_GM_Fatigue(ptr)),maximum
                receipt.before_used=tonumber(ffi.C.DNF_GM_UsedFatigue(ptr))
                receipt.before_premium,receipt.after_premium=tonumber(ffi.C.DNF_GM_PremiumFatigue(ptr)),premium
                receipt.state,receipt.note='unknown','apply_failed'
                local ok=pcall(function()
                    ffi.C.DNF_GM_SetFatigue(ptr,maximum)
                    ffi.C.DNF_GM_SetPremiumFatigue(ptr,premium)
                    ffi.C.DNF_GM_ResetUsedFatigue(ptr)
                    assert(tonumber(ffi.C.DNF_GM_Fatigue(ptr))==maximum and
                        tonumber(ffi.C.DNF_GM_PremiumFatigue(ptr))==premium and
                        tonumber(ffi.C.DNF_GM_UsedFatigue(ptr))==0,'fatigue readback differs')
                    ffi.C.DNF_GM_SendFatigue(ptr)
                    ffi.C.DNF_GM_SaveCharac(ptr)
                end)
                if ok then receipt.state,receipt.note='applied','save_queued' end
            else receipt.note='fatigue_limit' end
        elseif not money and not coin and not fatigue and not skills and not quest_points and not equipment_slots and not equipment_level and not weight and not level and not pvp and not ready_quest then receipt.note='unsupported_operation'
        elseif (money or coin) and user and user:GetState()>=3 and user:GetCharacNo()==tonumber(req.charac_no) then
            local inventory = user:GetCurCharacInvenR()
            if inventory then
                local get = money and ffi.C.DNF_GM_GetMoney or ffi.C.DNF_GM_GetCoin
                local set = money and ffi.C.DNF_GM_SetMoney or ffi.C.DNF_GM_SetCoin
                local before = tonumber(get(inventory.fptr))
                local after = tonumber(req.amount)
                if req.operation=='add_coin' or req.operation=='add_money' then after = before+after end
                local maximum = money and 2147483647 or 4294967295
                if after>=0 and after<=maximum and after%1==0 and (not money or before<=maximum) then
                    if money and after>before and ffi.C.DNF_GM_MoneyLimit(inventory.fptr,after-before)~=0 then
                        receipt.note='money_limit'
                    else
                        receipt.before,receipt.after = before,after
                        -- From this point a failure is ambiguous: do not label it
                        -- rejected or retry a reward whose memory change may exist.
                        receipt.state,receipt.note = 'unknown','apply_failed'
                        local ok = pcall(function()
                            set(inventory.fptr,after)
                            assert(tonumber(get(inventory.fptr))==after, 'inventory readback differs')
                            if money then assert(user:SendUpdateItemList(0,0), 'money update not sent')
                            else ffi.C.DNF_GM_SendCoin(user.fptr) end
                            assert(ffi.C.DNF_GM_SaveMoneyCoin(user.fptr)==1, 'save not queued')
                        end)
                        if ok then receipt.state,receipt.note = 'applied','save_queued' end
                    end
                else receipt.note = money and 'money_overflow' or 'coin_overflow' end
            else receipt.note = 'inventory_missing' end
        end
        if not waiting then state.receipts[#state.receipts+1] = receipt end
    end
end)
state.timer = uv.new_timer()
state.timer:start(100,500,function()
    if state.busy then return end
    for i=#state.billing,1,-1 do
        local pending=state.billing[i]
        local receipt=pending.receipt
        local ptr=world.FindUserByAcc(pending.mid)
        local user=ptr and game.fac.user(ptr)
        local finished=false
        if not user or user:GetState()<3 then
            receipt.note='billing_user_left';finished=true
        elseif tonumber(ffi.C.DNF_GM_GetCera(user.fptr))==receipt.after and
            tonumber(ffi.C.DNF_GM_GetCeraPoint(user.fptr))==receipt.after_point then
            local ok=pcall(ffi.C.DNF_GM_SendCash,user.fptr,false)
            receipt.state,receipt.note=ok and 'done' or 'unknown',ok and 'billing_refreshed' or 'billing_notify_failed'
            finished=true
        elseif uv.now()>=pending.deadline then
            receipt.note='billing_refresh_unconfirmed';finished=true
        end
        if finished then
            state.receipts[#state.receipts+1]=receipt
            table.remove(state.billing,i)
        end
    end
    config.owner,config.receipts = state.owner,state.receipts
    state.busy = true
    worker:queue(json.encode(config))
end)
_G.DNF_GM_ONLINE = state
_DP.add_unload_handler(function()
    state.stopping = true
    state.timer:stop()
    if not uv.is_closing(state.timer) then state.timer:close() end
end)
io.stderr:write('GM bridge installed; online character saves and billing refreshes require acknowledgement\n')
return state
