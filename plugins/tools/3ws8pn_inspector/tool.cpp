#include <boundary.hpp>
#include <imgui.h>
#include <srz80/tool.h>
#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
namespace
{
    struct P
    {
        uint32_t i;
        std::string n;
        uint64_t v;
    };
    struct T
    {
        const SrhToolHostV1 *h{};
        SrhHandle card{};
        std::vector<SrhToolCard> cards;
        std::vector<P> p;
    };
    bool refresh(T &t)
    {
        auto n = t.h->card_property_count(t.h->context, t.card);
        if (!n)
            return false;
        if (t.p.size() != n)
        {
            t.p.clear();
            for (uint32_t i = 0; i < n; i++)
            {
                SrhToolProperty x{SRH_INIT(SrhToolProperty)};
                if (t.h->card_property_info(t.h->context, t.card, i, &x) == SRH_OK)
                    t.p.push_back({i, x.name, 0});
            }
        }
        for (auto &x : t.p)
        {
            SrhValue v{SRH_INIT(SrhValue)};
            if (t.h->card_property_get(t.h->context, t.card, x.i, &v) == SRH_OK)
                x.v = v.unsigned_value;
        }
        return true;
    }
    SrhStatus SRH_CALL create(const SrhToolHostV1 *h, void **o)
    {
        return srz80::sdk::guard([&]()
                                 {if(!srz80::sdk::valid(h)||!o||!h->card_count||!h->card_info||!h->card_property_count||!h->card_property_info||!h->card_property_get)return SRH_INVALID;ImGui::SetAllocatorFunctions(h->imgui_alloc,h->imgui_free,h->imgui_allocator_context);ImGui::SetCurrentContext((ImGuiContext*)h->imgui_context);*o=new T{h};return SRH_OK; });
    }
    void SRH_CALL destroy(void *x) { delete (T *)x; }
    SrhStatus SRH_CALL draw(void *x, uint32_t *open)
    {
        return srz80::sdk::guard([&]()
                                 {auto&t=*(T*)x;bool vis=*open!=0;ImGui::SetCurrentContext((ImGuiContext*)t.h->imgui_context);if(ImGui::Begin("3WS8PN Inspector",&vis)){t.cards.clear();for(uint32_t i=0,n=t.h->card_count(t.h->context);i<n;i++){SrhToolCard c{SRH_INIT(SrhToolCard)};if(t.h->card_info(t.h->context,i,&c)==SRH_OK&&!std::strcmp(c.type,"3ws8pn"))t.cards.push_back(c);}if(t.cards.empty())ImGui::TextDisabled("No 3WS8PN cards");else{if(!t.card)t.card=t.cards[0].id;if(ImGui::BeginCombo("Card",std::to_string(t.card).c_str())){for(auto&c:t.cards)if(ImGui::Selectable(c.name,c.id==t.card))t.card=c.id;ImGui::EndCombo();}if(refresh(t)&&ImGui::BeginTable("channels",6,ImGuiTableFlags_Borders|ImGuiTableFlags_RowBg)){ImGui::TableSetupColumn("Channel");ImGui::TableSetupColumn("Frequency");ImGui::TableSetupColumn("Wave");ImGui::TableSetupColumn("Volume");ImGui::TableSetupColumn("Pan");ImGui::TableSetupColumn("Last Sample");ImGui::TableHeadersRow();for(int ch=0;ch<8;ch++){ImGui::TableNextRow();ImGui::TableSetColumnIndex(0);ImGui::Text("%d",ch+1);for(int f=0;f<5;f++){ImGui::TableSetColumnIndex(f+1);char n[32];std::snprintf(n,sizeof(n),"C%02d.%s",ch,f==0?"Frequency":f==1?"Waveform":f==2?"Volume":f==3?"Pan":"LastSample");auto it=std::find_if(t.p.begin(),t.p.end(),[&](auto&p){return p.n==n;});if(it!=t.p.end())ImGui::Text("%llu",(unsigned long long)it->v);}}ImGui::EndTable();}}}ImGui::End();*open=vis;return SRH_OK; });
    }
    const SrhToolPlugin api{SRH_INIT(SrhToolPlugin), "3ws8pn_inspector", "3WS8PN Inspector", IMGUI_VERSION, create, destroy, draw, "Audio", 0, nullptr, nullptr, nullptr};
}
extern "C" SRH_EXPORT const SrhToolPlugin *SRH_CALL srz80_tool_init(const SrhToolHostV1 *h) { return h ? &api : nullptr; }
