#pragma once
#include "source_model.hpp"
#include <algorithm>
#include <string>
namespace srz80::assembler {
// Undo transaction: one instruction edit or structural operation, not each
// individual keystroke.
struct LineDocument {
    std::vector<std::string> undo_stack, redo_stack;
    std::string transaction;
    bool active = false;
    static std::string leading_indentation(const std::string &line) {
        const auto first=line.find_first_not_of(" \t");
        const std::string indentation=line.substr(0,first==std::string::npos?line.size():first);
        std::string result;
        result.reserve(indentation.size());
        for(char c:indentation) {
            if(c=='\t') result.append(4,' ');
            else result.push_back(c);
        }
        return result;
    }
    static std::vector<std::string> lines(const std::string &text) {
        std::vector<std::string> result;
        size_t start=0;
        do {
            auto end=text.find('\n',start);
            if(end==std::string::npos) {result.push_back(text.substr(start));break;}
            result.push_back(text.substr(start,end-start));start=end+1;
        } while(start<=text.size());
        return result;
    }
    void begin(const SourceModel &s) {if(!active){transaction=s.text;active=true;}}
    void commit(const SourceModel &s) {
        if(active && transaction!=s.text) {undo_stack.push_back(transaction);redo_stack.clear();}
        active=false;
    }
    void cancel(SourceModel &s) {
        if(active && s.text!=transaction) {s.text=transaction;s.changed();}
        active=false;
    }
    void set_lines(SourceModel &s,const std::vector<std::string> &rows) {
        std::string text;
        for(size_t i=0;i<rows.size();++i) {if(i)text+='\n';text+=rows[i];}
        if(text!=s.text) {s.text=std::move(text);s.changed();}
    }
    void replace(SourceModel &s,size_t row,const std::string &text) {
        begin(s);auto rows=lines(s.text);rows[std::min(row,rows.size()-1)]=text;set_lines(s,rows);
    }
    void insert(SourceModel &s,size_t row,const std::string &text="") {
        commit(s);begin(s);auto rows=lines(s.text);
        rows.insert(rows.begin()+static_cast<ptrdiff_t>(std::min(row,rows.size())),text);
        set_lines(s,rows);commit(s);
    }
    void erase(SourceModel &s,size_t row) {
        commit(s);begin(s);auto rows=lines(s.text);
        rows.erase(rows.begin()+static_cast<ptrdiff_t>(std::min(row,rows.size()-1)));
        set_lines(s,rows);commit(s);
    }
    bool undo(SourceModel &s) {
        commit(s);if(undo_stack.empty())return false;
        redo_stack.push_back(s.text);s.text=std::move(undo_stack.back());undo_stack.pop_back();s.changed();return true;
    }
    bool redo(SourceModel &s) {
        commit(s);if(redo_stack.empty())return false;
        undo_stack.push_back(s.text);s.text=std::move(redo_stack.back());redo_stack.pop_back();s.changed();return true;
    }
    void clear() {undo_stack.clear();redo_stack.clear();active=false;}
};
}
