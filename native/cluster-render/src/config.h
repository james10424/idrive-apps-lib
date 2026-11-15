/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Robert Nisipeanu */
/* Settings: a key = value file, then -s key=value overrides in order.
 * key += a,b appends to the value instead: each entry not in it yet, after
 * a comma. Unknown keys are an error. The built-in keys and defaults are
 * cluster-demo's; a tool adds its own with define(), and a family of keys
 * it cannot list with extra_keys. */
#ifndef CLD_CONFIG_H
#define CLD_CONFIG_H

#include <functional>
#include <istream>
#include <map>
#include <string>

namespace cld {

class Config {
public:
    Config();

    /* Register an extra key and its default, for a tool that reuses this
     * Config with keys beyond cluster-demo's own. Idempotent; a key already
     * present keeps its value. Call before load()/set_pair(). */
    void define(const char *key, const char *def);

    /* Keys taken beyond the defined ones: a key no one defined is set,
     * from empty, when this says true for it (cluster-render's
     * id8_<target>_<field>, settings.h). Unset: none. */
    std::function<bool(const std::string &key)> extra_keys;

    bool load(const std::string &path, std::string *err);
    /* The same from a stream already open; name stands for it in errors. */
    bool load(std::istream &in, const std::string &name, std::string *err);
    bool set_pair(const std::string &pair, std::string *err);

    std::string str(const char *key) const;
    long num(const char *key) const;
    /* "w x h" or "wxh"; false when unset or malformed. */
    bool size(const char *key, unsigned *w, unsigned *h) const;
    /* An unset value (empty) for slot keys means "not configured". */
    bool has(const char *key) const;
    /* Every key with its value. */
    const std::map<std::string, std::string> &values() const { return values_; }

private:
    bool set(const std::string &key, const std::string &value, bool append, std::string *err);
    std::map<std::string, std::string> values_;
};

}  // namespace cld

#endif
