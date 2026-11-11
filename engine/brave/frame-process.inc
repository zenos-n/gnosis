/* SPDX-License-Identifier: GPL-3.0-or-later; included inside Process.
 * OOPIF protocol sessions belong to their attached GTK page, never to whichever
 * window is active. Runtime context IDs are session-local, so private worlds
 * receive synthetic negative IDs before entering the page's menu bookkeeping.
 */
  struct FrameSession {
    std::weak_ptr<Page> owner;
    std::string parent;
    std::map<int,int> contexts;
    std::set<std::string> frames;
    guint resume_timer = 0;
  };
  std::map<std::string,FrameSession> frame_sessions;
  std::map<std::string,std::string> frame_sources;
  int frame_context_sequence = 0;

  void resume_frame(const std::string& source) {
    auto found = frame_sessions.find(source);
    if (found == frame_sessions.end()) return;
    if (found->second.resume_timer) g_source_remove(std::exchange(found->second.resume_timer,0));
    command("Runtime.runIfWaitingForDebugger","{}",source);
  }
  void clear_frame_contexts(const std::string& source) {
    auto found = frame_sessions.find(source);
    if (found == frame_sessions.end()) return;
    if (auto page = found->second.owner.lock()) {
      if (!found->second.contexts.empty() && (page->state_->close_approved || page->state_->close_callback)) page->cancel_close();
      page->dismiss_context_menu();
      for (auto [runtime,identifier] : found->second.contexts) {
        g_autoptr(JsonObject) message = json_object_new();
        json_object_set_int_member(message,"executionContextId",identifier);
        page->handle("Runtime.executionContextDestroyed",message,true);
      }
    }
    found->second.contexts.clear();
  }
  void forget_frame(const std::string& source) {
    std::vector<std::string> children;
    for (auto& [id,frame] : frame_sessions) if (frame.parent == source) children.push_back(id);
    for (auto& id : children) forget_frame(id);
    auto found = frame_sessions.find(source);
    if (found == frame_sessions.end()) return;
    clear_frame_contexts(source);
    if (found->second.resume_timer) g_source_remove(found->second.resume_timer);
    for (auto& frame : found->second.frames) {
      auto entry = frame_sources.find(frame);
      if (entry != frame_sources.end() && entry->second == source) {
        frame_sources.erase(entry); frame_owners.erase(frame);
        if (auto page=found->second.owner.lock()) page->state_->theme_frames.erase(frame);
      }
    }
    frame_sessions.erase(found);
  }
  void forget_frames(Page* owner = nullptr) {
    std::vector<std::string> sources;
    for (auto& [id,frame] : frame_sessions) {
      auto page = frame.owner.lock();
      if (!owner || !page || page.get() == owner) sources.push_back(id);
    }
    for (auto& id : sources) forget_frame(id);
  }
  void attach_frames(const std::string& source) {
    if (!adapter_enabled("frame-sessions")) return;
    // Native menus/metadata can be installed with runImmediately below. Pausing
    // every cross-origin frame puts normal site startup behind adapter replies
    // (or the five-second fallback), which breaks time-sensitive embeds.
    command("Target.setAutoAttach",R"({"autoAttach":true,"waitForDebuggerOnStart":false,"flatten":true,"filter":[{"type":"iframe","exclude":false},{"exclude":true}]})",source,
      [](JsonObject*,const std::string& error){if(!error.empty())g_warning("Native iframe adapters could not attach: %s",error.c_str());});
  }
  bool frame_event(const std::string& source,const std::string& method,JsonObject* params) {
    if (method == "Target.detachedFromTarget") {
      const std::string child = text(params,"sessionId");
      if (!frame_sessions.contains(child)) return false;
      forget_frame(child); return true;
    }
    if (method == "Target.attachedToTarget") {
      std::shared_ptr<Page> page;
      if (auto main = sessions.find(source); main != sessions.end()) page = main->second.lock();
      else if (auto parent = frame_sessions.find(source); parent != frame_sessions.end()) page = parent->second.owner.lock();
      const std::string child = text(params,"sessionId");
      auto* info = object(params,"targetInfo");
      if (std::string(text(info,"type")) != "iframe") return false;
      if (!page) {
        // An attachment can outlive its owning tab during close/reuse. Never
        // leave a target waiting for a debugger that no longer owns it.
        if (!child.empty()) {
          command("Runtime.runIfWaitingForDebugger","{}",child);
          command("Target.detachFromTarget","{\"sessionId\":"+quote(child)+"}",source);
        }
        return true;
      }
      if (child.empty()) return true;
      if (page->is_closed() || std::string(text(info,"type")) != "iframe" || frame_sessions.size() >= 512) {
        command("Runtime.runIfWaitingForDebugger","{}",child);
        command("Target.detachFromTarget","{\"sessionId\":"+quote(child)+"}",source);
        return true;
      }
      if (frame_sessions.contains(child)) return true;
      auto& frame = frame_sessions[child]; frame.owner = page; frame.parent = source;
      struct Resume { std::weak_ptr<Process> process; std::string source; };
      frame.resume_timer = g_timeout_add_full(G_PRIORITY_DEFAULT,5000,+[](gpointer data)->gboolean {
        auto* pending = static_cast<Resume*>(data);
        if (auto process = pending->process.lock()) {
          auto found = process->frame_sessions.find(pending->source);
          if (found != process->frame_sessions.end()) found->second.resume_timer = 0;
          process->resume_frame(pending->source);
        }
        return G_SOURCE_REMOVE;
      },new Resume{weak_from_this(),child},+[](gpointer data){delete static_cast<Resume*>(data);});
      attach_frames(child);
      command("Emulation.setEmulatedMedia", web_color_scheme(), child);
      command("Page.enable","{}",child); command("Network.enable","{}",child);
      command("Page.getFrameTree","{}",child,[weak=weak_from_this(),child](JsonObject* result,const std::string&) {
        auto process=weak.lock();if(!process||!result)return;
        auto found=process->frame_sessions.find(child);if(found==process->frame_sessions.end())return;
        auto page=found->second.owner.lock();if(!page||page->is_closed())return;
        std::function<void(JsonObject*)> register_tree=[&](JsonObject* tree) {
          auto* frame=object(tree,"frame");const std::string id=text(frame,"id");
          if(!id.empty()) {
            found->second.frames.insert(id);process->frame_owners[id]=page;process->frame_sources[id]=child;
            page->theme_document(id,text(frame,"loaderId"));
          }
          auto* nested=tree&&json_object_has_member(tree,"childFrames")?json_object_get_array_member(tree,"childFrames"):nullptr;
          for(guint i=0;nested&&i<json_array_get_length(nested);++i)register_tree(json_array_get_object_element(nested,i));
        };
        register_tree(object(result,"frameTree"));
      });
      command("Runtime.addBinding",R"({"name":"__gnosisContextMenu","executionContextName":"GnosisContextMenu"})",child);
      command("Runtime.addBinding",R"({"name":"__gnosisDocumentMetadata","executionContextName":"GnosisDocumentMetadata"})",child);
      command("Runtime.enable","{}",child);
      for (auto item : {std::pair{"GnosisContextMenu","context-menu.js"}}) {
        if (!adapter_enabled("context-menu")) continue;
        g_autofree char* path = g_build_filename(GNOSIS_NATIVE_INTEGRATION_DIR,item.second,nullptr);
        g_autofree char* script = nullptr;
        if (g_file_get_contents(path,&script,nullptr,nullptr))
          command("Page.addScriptToEvaluateOnNewDocument","{\"worldName\":"+quote(item.first)+",\"source\":"+quote(script)+",\"runImmediately\":true}",child);
      }
      if (adapter_enabled("document-metadata")) command("Page.addScriptToEvaluateOnNewDocument","{\"worldName\":\"GnosisDocumentMetadata\",\"source\":"+quote(document_adapter_source())+",\"runImmediately\":true}",child);
      // Release old paused attachments as well, without waiting on a renderer
      // reply or depending on the document-metadata adapter being enabled.
      resume_frame(child);
      return true;
    }
    auto found = frame_sessions.find(source);
    if (found == frame_sessions.end()) return false;
    auto page = found->second.owner.lock();
    if (!page || page->is_closed()) { resume_frame(source); forget_frame(source); return true; }
    auto& frame = found->second;
    if (method == "Page.frameNavigated" || method == "Page.frameAttached") {
      if (page->state_->close_approved || page->state_->close_callback) page->cancel_close();
      auto* details = object(params,"frame");
      const std::string id = details ? text(details,"id") : text(params,"frameId");
      if (!id.empty()) {
        frame.frames.insert(id); frame_owners[id]=page; frame_sources[id]=source;
        if (details) page->theme_document(id,text(details,"loaderId"));
      }
    } else if (method == "Page.frameDetached" && std::string(text(params,"reason")) == "remove") {
      const std::string id = text(params,"frameId"); frame.frames.erase(id);
      auto owner=frame_sources.find(id);
      if (owner!=frame_sources.end()&&owner->second==source) { frame_sources.erase(owner); frame_owners.erase(id); }
    } else if (method == "CSS.styleSheetRemoved") {
      page->handle(method,params,true);
    } else if (method == "Runtime.executionContextsCleared") {
      clear_frame_contexts(source);
    } else if (method == "Runtime.executionContextCreated") {
      auto* context=object(params,"context");
      const std::string name=text(context,"name");
      if (name != "GnosisContextMenu" && name != "GnosisDocumentMetadata") return true;
      const int runtime=json_object_get_int_member_with_default(context,"id",0);
      if (runtime <= 0 || frame_context_sequence == G_MAXINT) return true;
      const std::string frame_id=text(object(context,"auxData"),"frameId");
      if (!frame_id.empty()) { frame.frames.insert(frame_id); frame_owners[frame_id]=page; frame_sources[frame_id]=source; }
      auto previous=frame.contexts.find(runtime);
      if (previous!=frame.contexts.end()) {
        g_autoptr(JsonObject) removed=json_object_new();
        json_object_set_int_member(removed,"executionContextId",previous->second);
        page->handle("Runtime.executionContextDestroyed",removed,true);
      }
      const int identifier=-(++frame_context_sequence);
      frame.contexts[runtime]=identifier;
      page->state_->menu_sessions[identifier]=source;
      json_object_set_int_member(context,"id",identifier);
      page->handle(method,params,true);
    } else if (method == "Runtime.executionContextDestroyed" || method == "Runtime.bindingCalled") {
      const int runtime=json_object_get_int_member_with_default(params,"executionContextId",0);
      auto context=frame.contexts.find(runtime);
      if (context == frame.contexts.end()) return true;
      const int identifier=context->second;
      if (method == "Runtime.executionContextDestroyed") frame.contexts.erase(context);
      json_object_set_int_member(params,"executionContextId",identifier);
      page->handle(method,params,true);
    }
    // Child navigation/load/security events must never replace top-level state
    // or enter the main session's JavaScript evaluation context registry.
    return true;
  }
