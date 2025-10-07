/referenced from -> https://github.com/jialand/TheMuteLift/blob/main/Dialogue.hpp
//@ChatGPT used to create this file
#include "Dialogue.hpp"
#include <fstream>
#include <sstream>
#include <string>
#include <cctype>

static inline std::string trim(std::string s){
    size_t a=0,b=s.size();
    while(a<b && std::isspace((unsigned char)s[a])) ++a;
    while(b>a && std::isspace((unsigned char)s[b-1])) --b;
    return s.substr(a,b-a);
}

const DialogueNode* DialogueGraph::get(const std::string& id) const {
    auto it = nodes.find(id);
    if(it==nodes.end()) return nullptr;
    return &it->second;
}

bool DialogueGraph::load_from_file(const std::string& path, std::string* err){
    nodes.clear();
    start_id.clear();

    std::ifstream fin(path, std::ios::binary);
    if(!fin){
        if(err) *err = "Failed to open: " + path;
        return false;
    }

    std::string line;
    DialogueNode cur;
    bool in_state = false;
    int line_no = 0;
    auto flush_state = [&](){
        if(in_state){
            if(cur.id.empty()){
                // invalid; ignore
            }else{
                nodes[cur.id] = cur;
            }
            cur = DialogueNode{};
            in_state = false;
        }
    };

    while(std::getline(fin, line)){
        ++line_no;
        // handle CRLF
        if(!line.empty() && line.back()=='\r') line.pop_back();

        std::string t = trim(line);
        if(t.empty()) continue;
        if(t.rfind("//",0)==0 || t.rfind("#",0)==0) continue;

        if(t.rfind("start:",0)==0){
            start_id = trim(t.substr(6));
            continue;
        }

        if(t.rfind("state:",0)==0){
            flush_state();
            cur = DialogueNode{};
            cur.id = trim(t.substr(6));
            in_state = true;
            continue;
        }

        if(t == "text:" ){
            // must read <<< then lines until >>>
            std::string open;
            if(!std::getline(fin, open)){ if(err) *err="Unexpected EOF after text: (line "+std::to_string(line_no)+")"; return false; }
            ++line_no; if(!open.empty() && open.back()=='\r') open.pop_back();
            if(trim(open) != "<<<"){ if(err) *err="Expect <<< after text: at line "+std::to_string(line_no); return false; }

            std::ostringstream oss;
            std::string tl;
            while(std::getline(fin, tl)){
                ++line_no; if(!tl.empty() && tl.back()=='\r') tl.pop_back();
                if(trim(tl) == ">>>") break;
                oss << tl << "\n";
            }
            cur.text = oss.str();
            // remove one trailing '\n' if present
            if(!cur.text.empty() && cur.text.back()=='\n') cur.text.pop_back();
            continue;
        }

        if(t.rfind("option:",0)==0){
            std::string rest = trim(t.substr(7));
            // format: <label> -> <next>
            size_t arrow = rest.find("->");
            if(arrow == std::string::npos){
                if(err) *err = "Bad option (missing '->') at line " + std::to_string(line_no);
                return false;
            }
            std::string label = trim(rest.substr(0,arrow));
            std::string next  = trim(rest.substr(arrow+2));
            if(label.empty() || next.empty()){
                if(err) *err = "Empty label or next at line " + std::to_string(line_no);
                return false;
            }
            cur.options.push_back(DialogueOption{label,next});
            continue;
        }

        if(t=="endstate"){
            flush_state();
            continue;
        }

        // unknown directive inside a state is allowed to be ignored,
        // but here we are strict:
        // if(err) *err = "Unknown line at " + std::to_string(line_no) + ": " + t;
        // return false;
    }

    flush_state();

    if(start_id.empty()){
        // use first state if available
        if(!nodes.empty()) start_id = nodes.begin()->first;
    }
    if(start_id.empty()){
        if(err) *err = "No states loaded.";
        return false;
    }

    return true;
}