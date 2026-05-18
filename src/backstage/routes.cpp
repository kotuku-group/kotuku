// Auto-generated source code.  Regenerate with the backstage_routes CMake target.

#include "routes/agents.cpp"
#include "routes/ping.cpp"
#include "routes/objects.cpp"
#include "routes/classes.cpp"
#include "routes/modules.cpp"
#include "routes/errors.cpp"
#include "routes/diagnostics.cpp"
#include "routes/jobs.cpp"
#include "routes/logs.cpp"
#include "routes/subscriptions.cpp"
#include "routes/docs.cpp"
#include "routes/scripts.cpp"

static std::array<BackstageRoute, 31> glRoutes = {
   BackstageRoute("GET", "/agents/context", "^/agents/context$", get_agents_context),
   BackstageRoute("GET", "/ping", "^/ping$", get_ping),
   BackstageRoute("GET", "/objects", "^/objects$", get_objects),
   BackstageRoute("POST", "/objects", "^/objects$", post_objects),
   BackstageRoute("GET", "/objects/{uid}", "^/objects/(-?[0-9]+)$", get_objects_uid),
   BackstageRoute("GET", "/objects/{uid}/children", "^/objects/(-?[0-9]+)/children$", get_objects_uid_children),
   BackstageRoute("GET", "/objects/{uid}/subscribers", "^/objects/(-?[0-9]+)/subscribers$", get_objects_uid_subscribers),
   BackstageRoute("POST", "/objects/{uid}", "^/objects/(-?[0-9]+)$", post_objects_uid),
   BackstageRoute("GET", "/classes", "^/classes$", get_classes),
   BackstageRoute("GET", "/classes/{class}", "^/classes/([^\\/]+)$", get_classes_class),
   BackstageRoute("GET", "/classes/{class}/fields", "^/classes/([^\\/]+)/fields$", get_classes_class_fields),
   BackstageRoute("GET", "/classes/{class}/actions", "^/classes/([^\\/]+)/actions$", get_classes_class_actions),
   BackstageRoute("GET", "/classes/{class}/methods", "^/classes/([^\\/]+)/methods$", get_classes_class_methods),
   BackstageRoute("GET", "/modules", "^/modules$", get_modules),
   BackstageRoute("GET", "/modules/{name}", "^/modules/([^\\/]+)$", get_modules_name),
   BackstageRoute("GET", "/errors", "^/errors$", get_errors),
   BackstageRoute("GET", "/errors/{code}", "^/errors/([^\\/]+)$", get_errors_code),
   BackstageRoute("GET", "/diagnostics/memory", "^/diagnostics/memory$", get_diagnostics_memory),
   BackstageRoute("GET", "/diagnostics/timers", "^/diagnostics/timers$", get_diagnostics_timers),
   BackstageRoute("GET", "/jobs/{job}", "^/jobs/([^\\/]+)$", get_jobs_job),
   BackstageRoute("DELETE", "/jobs/{job}", "^/jobs/([^\\/]+)$", delete_jobs_job),
   BackstageRoute("POST", "/logs/level", "^/logs/level$", post_logs_level),
   BackstageRoute("POST", "/logs/start", "^/logs/start$", post_logs_start),
   BackstageRoute("POST", "/logs/stop", "^/logs/stop$", post_logs_stop),
   BackstageRoute("GET", "/logs", "^/logs$", get_logs),
   BackstageRoute("POST", "/subscriptions", "^/subscriptions$", post_subscriptions),
   BackstageRoute("GET", "/subscriptions/{id}", "^/subscriptions/([^\\/]+)$", get_subscriptions_id),
   BackstageRoute("DELETE", "/subscriptions/{id}", "^/subscriptions/([^\\/]+)$", delete_subscriptions_id),
   BackstageRoute("GET", "/docs", "^/docs$", get_docs),
   BackstageRoute("GET", "/docs/routes", "^/docs/routes$", get_docs_routes),
   BackstageRoute("POST", "/scripts", "^/scripts$", post_scripts)
};
