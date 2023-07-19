/*
Copyright (C) 2023 The Falco Authors.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.

*/

#include "plugin.h"

#define __CATCH_ERR_MSG(_ERR, _F) \
{ \
	try { _F; } \
	catch (const std::exception& _e) { _ERR = _e.what(); } \
	catch (...) { _ERR = "unknown error"; } \
}

#define __PLUGIN_STATETYPE_SWITCH(_kt) \
{ \
	switch (_kt) \
	{ \
		case ss_plugin_state_type::SS_PLUGIN_ST_INT8: \
			_X(int8_t, s8); break; \
		case ss_plugin_state_type::SS_PLUGIN_ST_INT16: \
			_X(int16_t, s16); break; \
		case ss_plugin_state_type::SS_PLUGIN_ST_INT32: \
			_X(int32_t, s32); break; \
		case ss_plugin_state_type::SS_PLUGIN_ST_INT64: \
			_X(int64_t, s64); break; \
		case ss_plugin_state_type::SS_PLUGIN_ST_UINT8: \
			_X(uint8_t, u8); break; \
		case ss_plugin_state_type::SS_PLUGIN_ST_UINT16: \
			_X(uint16_t, u16); break; \
		case ss_plugin_state_type::SS_PLUGIN_ST_UINT32: \
			_X(uint32_t, u32); break; \
		case ss_plugin_state_type::SS_PLUGIN_ST_UINT64: \
			_X(uint64_t, u64); break; \
		case ss_plugin_state_type::SS_PLUGIN_ST_STRING: \
			_X(std::string, str); break; \
		case ss_plugin_state_type::SS_PLUGIN_ST_BOOL: \
			_X(bool, b); break; \
		default: \
			throw sinsp_exception("can't convert plugin state type to typeinfo: " + std::to_string(_kt)); \
	} \
}

static inline ss_plugin_state_type typeinfo_to_state_type(const libsinsp::state::typeinfo& i)
{
    switch(i.index())
    {
		case libsinsp::state::typeinfo::index_t::PT_INT8:
			return ss_plugin_state_type::SS_PLUGIN_ST_INT8;
		case libsinsp::state::typeinfo::index_t::PT_INT16:
			return ss_plugin_state_type::SS_PLUGIN_ST_INT16;
		case libsinsp::state::typeinfo::index_t::PT_INT32:
			return ss_plugin_state_type::SS_PLUGIN_ST_INT32;
		case libsinsp::state::typeinfo::index_t::PT_INT64:
			return ss_plugin_state_type::SS_PLUGIN_ST_INT64;
		case libsinsp::state::typeinfo::index_t::PT_UINT8:
			return ss_plugin_state_type::SS_PLUGIN_ST_UINT8;
		case libsinsp::state::typeinfo::index_t::PT_UINT16:
			return ss_plugin_state_type::SS_PLUGIN_ST_UINT16;
		case libsinsp::state::typeinfo::index_t::PT_UINT32:
			return ss_plugin_state_type::SS_PLUGIN_ST_UINT32;
		case libsinsp::state::typeinfo::index_t::PT_UINT64:
			return ss_plugin_state_type::SS_PLUGIN_ST_UINT64;
		case libsinsp::state::typeinfo::index_t::PT_CHARBUF:
			return ss_plugin_state_type::SS_PLUGIN_ST_STRING;
		case libsinsp::state::typeinfo::index_t::PT_BOOL:
			return ss_plugin_state_type::SS_PLUGIN_ST_BOOL;
        default:
            throw sinsp_exception("can't convert typeinfo to plugin state type: " + std::string(i.name()));
    }
}

template<typename From, typename To> static inline void convert_types(const From& from, To& to)
{
	to = from;
}

// special cases for strings
template<> inline void convert_types(const std::string& from, const char*& to)
{
	to = from.c_str();
}
template<> inline void convert_types(const char* const& from, std::string& to)
{
	if (!from || *from == '\0')
	{
		to = "";
	}
	else
	{
		to = from;
	}
}

// wraps instances of ss_plugin_table_input and makes them comply
// to the libsinsp::state::table state tables definition.
template <typename KeyType>
struct plugin_table_wrapper: public libsinsp::state::table<KeyType>
{
	using ss = libsinsp::state::static_struct;

	using ds = libsinsp::state::dynamic_struct;
	
	struct plugin_field_infos: public ds::field_infos
	{
		plugin_field_infos(
			const sinsp_plugin* o,
			const std::shared_ptr<ss_plugin_table_input>& i)
				: field_infos(), m_owner(o), m_input(i), m_accessors() {};
        plugin_field_infos(plugin_field_infos&&) = default;
        plugin_field_infos& operator = (plugin_field_infos&&) = default;
        plugin_field_infos(const plugin_field_infos& s) = delete;
        plugin_field_infos& operator = (const plugin_field_infos& s) = delete;
		virtual ~plugin_field_infos() = default;

		const sinsp_plugin* m_owner;
		std::shared_ptr<ss_plugin_table_input> m_input;
		std::vector<ss_plugin_table_field_t*> m_accessors;

		virtual const std::unordered_map<std::string, ds::field_info>& fields() override
        {
			uint32_t nfields = 0;
			auto res = m_input->fields.list_table_fields(m_input->table, &nfields);
			if (res == NULL)
			{
				throw sinsp_exception("plugin table list fields error: " + m_owner->get_last_error());
			}
			if (nfields != ds::field_infos::fields().size())
			{
				for (uint32_t i = 0; i < nfields; i++)
				{
					ds::field_info f;
					#define _X(_type, _dtype) \
					{ \
						f = ds::field_info::build<_type>(res[i].name, i, this, res[i].read_only); \
					}
					__PLUGIN_STATETYPE_SWITCH(res[i].field_type);
					#undef _X
					ds::field_infos::add_field(f);
				}
			}

			// make sure we have accessors for all of these fields
            const auto& ret = ds::field_infos::fields();
			for (const auto& it : ret)
			{
				const auto& f = it.second;
				while (m_accessors.size() <= f.index())
				{
					m_accessors.push_back(nullptr);
				}
				if (m_accessors[f.index()] == nullptr)
				{
					auto facc = m_input->fields.get_table_field(m_input->table, f.name().c_str(), typeinfo_to_state_type(f.info()));
					if (facc == NULL)
					{
						throw sinsp_exception("plugin table get field error: " + m_owner->get_last_error());
					}
					m_accessors[f.index()] = facc;
				}
			}
			return ret;
        }

		virtual const ds::field_info& add_field(const ds::field_info& field) override
		{
			auto ret = m_input->fields.add_table_field(m_input->table, field.name().c_str(), typeinfo_to_state_type(field.info()));
			if (ret == NULL)
			{
				throw sinsp_exception("plugin table list fields error: "  + m_owner->get_last_error());
			}
			// trigger all updates and obtain the right ref
			this->fields();
			return ds::field_infos::add_field(field);
		}
	};

	struct plugin_table_entry: public libsinsp::state::table_entry
	{
		plugin_table_entry(
			const sinsp_plugin* o,
			const std::shared_ptr<ss_plugin_table_input>& i,
			const std::shared_ptr<plugin_field_infos>& fields,
			ss_plugin_table_entry_t* e,
			bool destroy)
				: table_entry(fields),
				m_owner(o),
				m_input(i),
				m_entry(e),
				m_destroy_entry(destroy) {};
		plugin_table_entry(const plugin_table_entry& o)
		{
			// note: this is not supposed to ever happen: in the general case,
			// m_destroy_entry will be false because plugin_table_entry will not
			// own the entry (the table will). The only time in which it is owner
			// is when being held in a unique pointer during the
			// new_entry -> add_entry flow.
			ASSERT(!o.m_destroy_entry);
			if (o.m_destroy_entry)
			{
				throw sinsp_exception("plugin_table_entry can't be copied while being owner of an entry pointer");
			}
			m_owner = o.m_owner;
			m_input = o.m_input;
			m_entry = o.m_entry;
			m_destroy_entry = o.m_destroy_entry;
		};
        plugin_table_entry& operator = (const plugin_table_entry& o)
		{
			ASSERT(!o.m_destroy_entry);
			if (o.m_destroy_entry)
			{
				throw sinsp_exception("plugin_table_entry can't be copied while being owner of an entry pointer");
			}
			m_owner = o.m_owner;
			m_input = o.m_input;
			m_entry = o.m_entry;
			m_destroy_entry = o.m_destroy_entry;
		}
        plugin_table_entry(plugin_table_entry&& o)
		{
			m_owner = o.m_owner;
			m_input = o.m_input;
			m_entry = o.m_entry;
			m_destroy_entry = o.m_destroy_entry;
			o.m_destroy_entry = false;
		};
        plugin_table_entry& operator = (plugin_table_entry&& o)
		{
			m_owner = o.m_owner;
			m_input = o.m_input;
			m_entry = o.m_entry;
			m_destroy_entry = o.m_destroy_entry;
			o.m_destroy_entry = false;
			return *this;
		};
		virtual ~plugin_table_entry()
		{
			if (m_destroy_entry)
			{
				m_input->writer.destroy_table_entry(m_input->table, m_entry);
			}
		};

		const sinsp_plugin* m_owner;
		std::shared_ptr<ss_plugin_table_input> m_input;
		ss_plugin_table_entry_t* m_entry;
		bool m_destroy_entry;

		// note(jasondellaluce): dynamic cast is expensive but this is not expected
		// to ever be invoked because we plan of settings the fields shared pointer
		// at construction time. This is just here as a consistency fence in
		// case of misuse.
		virtual void set_dynamic_fields(const std::shared_ptr<ds::field_infos>& defs) override
		{
			if (defs && dynamic_cast<plugin_field_infos*>(defs.get()) == nullptr)
			{
				throw sinsp_exception("plugin table can only be set with plugin dynamic fields");
			}
			table_entry::set_dynamic_fields(defs);
		}

		virtual void get_dynamic_field(const ds::field_info& i, void* out) override
		{
			const auto& infos = get_plugin_field_infos();
			ss_plugin_state_data dout;
			auto rc = m_input->reader.read_entry_field(m_input->table, m_entry, infos.m_accessors[i.index()], &dout);
			if (rc != SS_PLUGIN_SUCCESS)
			{
				throw sinsp_exception("plugin table entry read field error: " + m_owner->get_last_error());
			}
			#define _X(_type, _dtype) \
			{ \
				convert_types(dout._dtype, *(_type*) out); \
			}
			__PLUGIN_STATETYPE_SWITCH(typeinfo_to_state_type(i.info()));
			#undef _X
		}

		virtual void set_dynamic_field(const ds::field_info& i, const void* in) override
		{
			const auto& infos = get_plugin_field_infos();
			ss_plugin_state_data v;

			#define _X(_type, _dtype) \
			{ \
				convert_types(*(_type*) in, v._dtype); \
			}
			__PLUGIN_STATETYPE_SWITCH(typeinfo_to_state_type(i.info()));
			#undef _X

			auto rc = m_input->writer.write_entry_field(m_input->table, m_entry, infos.m_accessors[i.index()], &v);
			if (rc != SS_PLUGIN_SUCCESS)
			{
				throw sinsp_exception("plugin table entry write field error: " + m_owner->get_last_error());
			}
		}
	private:
		const plugin_field_infos& get_plugin_field_infos() const
		{
			if (dynamic_fields() == nullptr)
			{
				throw sinsp_exception("plugin table entry fields definitions are not set");
			}
			// note: casting is safe because we force the plugin_field_infos
			// subtype both the constructor and the setter
			return *static_cast<plugin_field_infos*>(dynamic_fields().get());
		}
	};

	plugin_table_wrapper(const sinsp_plugin* o, const ss_plugin_table_input* i)
        : libsinsp::state::table<KeyType>(i->name, ss::field_infos()),
		  m_owner(o),
		  m_input(std::make_shared<ss_plugin_table_input>(*i)),
		  m_static_fields(),
		  m_dyn_fields(std::make_shared<plugin_field_infos>(o, m_input))
    {
        auto t = libsinsp::state::typeinfo::of<KeyType>();
        if (m_input->key_type != typeinfo_to_state_type(t))
        {
            throw sinsp_exception("invalid key type for plugin-owned table: " + std::string(t.name()));
        }
    }

    virtual ~plugin_table_wrapper() = default;
    plugin_table_wrapper(plugin_table_wrapper&&) = default;
    plugin_table_wrapper& operator = (plugin_table_wrapper&&) = default;
    plugin_table_wrapper(const plugin_table_wrapper& s) = delete;
    plugin_table_wrapper& operator = (const plugin_table_wrapper& s) = delete;

	const sinsp_plugin* m_owner;
	std::shared_ptr<ss_plugin_table_input> m_input;
	libsinsp::state::static_struct::field_infos m_static_fields;
	std::shared_ptr<plugin_field_infos> m_dyn_fields;

	inline std::string invalid_access_msg(const std::string& op) const
	{
		return "operation '" + op + "' not supported by plugin-owned table '" + this->name() + "'";
	}

	const libsinsp::state::static_struct::field_infos& static_fields() const override
    {
		// note: always empty, plugin-defined table have no "static" fields,
		// all of them are dynamically-discovered at runtime
        return m_static_fields;
    }

    std::shared_ptr<ds::field_infos> dynamic_fields() const override
    {
        return m_dyn_fields;
    }

	size_t entries_count() const override
	{
		auto res = m_input->reader.get_table_size(m_input->table);
		if (res == (uint64_t) -1)
		{
			throw sinsp_exception(m_owner->get_last_error());
		}
		return (size_t) res;
	}

	void clear_entries() override
	{
		auto res = m_input->writer.clear_table(m_input->table);
		if (res != SS_PLUGIN_SUCCESS)
		{
			throw sinsp_exception(m_owner->get_last_error());
		}
	}

	bool foreach_entry(std::function<bool(libsinsp::state::table_entry& e)> pred) override
	{
		throw sinsp_exception(invalid_access_msg("foreach"));
	}

    std::unique_ptr<libsinsp::state::table_entry> new_entry() const override
	{
		auto res = m_input->writer.create_table_entry(m_input->table);
		if (res == NULL)
		{
			throw sinsp_exception(m_owner->get_last_error());
		}
		return std::unique_ptr<libsinsp::state::table_entry>(new plugin_table_entry(m_owner, m_input, m_dyn_fields, res, true));
	}

	std::shared_ptr<libsinsp::state::table_entry> get_entry(const KeyType& key) override
	{
		ss_plugin_state_data keydata;
		get_key_data(key, keydata);
		auto res = m_input->reader.get_table_entry(m_input->table, &keydata);
		if (res == NULL)
		{
			return nullptr;
		}
		return std::shared_ptr<libsinsp::state::table_entry>(new plugin_table_entry(m_owner, m_input, m_dyn_fields, res, false));
	}

    std::shared_ptr<libsinsp::state::table_entry> add_entry(const KeyType& key, std::unique_ptr<libsinsp::state::table_entry> e) override
	{
		ASSERT(dynamic_cast<plugin_table_entry*>(e.get()) != nullptr);
		plugin_table_entry* entry = static_cast<plugin_table_entry*>(e.get());
		ss_plugin_state_data keydata;
		get_key_data(key, keydata);
		auto res = m_input->writer.add_table_entry(m_input->table, &keydata, entry->m_entry);
		if (res == NULL)
		{
			throw sinsp_exception(m_owner->get_last_error());
		}
		entry->m_entry = res;
		entry->m_destroy_entry = false;
		return std::move(e);
	}

    bool erase_entry(const KeyType& key) override
	{
		ss_plugin_state_data keydata;
		get_key_data(key, keydata);
		auto res = m_input->writer.erase_table_entry(m_input->table, &keydata);
		return res == SS_PLUGIN_SUCCESS;
	}

	private:
	void get_key_data(const KeyType& key, ss_plugin_state_data& out);
};

template<> void plugin_table_wrapper<int8_t>::get_key_data(const int8_t& key, ss_plugin_state_data& out)
{
	out.s8 = key;
}

template<> void plugin_table_wrapper<int16_t>::get_key_data(const int16_t& key, ss_plugin_state_data& out)
{
	out.s16 = key;
}

template<> void plugin_table_wrapper<int32_t>::get_key_data(const int32_t& key, ss_plugin_state_data& out)
{
	out.s32 = key;
}

template<> void plugin_table_wrapper<int64_t>::get_key_data(const int64_t& key, ss_plugin_state_data& out)
{
	out.s64 = key;
}

template<> void plugin_table_wrapper<uint8_t>::get_key_data(const uint8_t& key, ss_plugin_state_data& out)
{
	out.u8 = key;
}

template<> void plugin_table_wrapper<uint16_t>::get_key_data(const uint16_t& key, ss_plugin_state_data& out)
{
	out.u16 = key;
}

template<> void plugin_table_wrapper<uint32_t>::get_key_data(const uint32_t& key, ss_plugin_state_data& out)
{
	out.u32 = key;
}

template<> void plugin_table_wrapper<uint64_t>::get_key_data(const uint64_t& key, ss_plugin_state_data& out)
{
	out.u64 = key;
}

template<> void plugin_table_wrapper<std::string>::get_key_data(const std::string& key, ss_plugin_state_data& out)
{
	out.str = key.c_str();
}

template<> void plugin_table_wrapper<bool>::get_key_data(const bool& key, ss_plugin_state_data& out)
{
	out.b = key;
}

// wraps instances of libsinsp::state::table and makes them comply
// to the plugin API state tables definitions.
struct sinsp_table_wrapper
{
	// wraps a dynamic or a static field accessor and its type
    struct field_accessor_wrapper
    {
		void* accessor;
		bool dynamic;
		ss_plugin_state_type data_type;
    };

	template <typename T>
	explicit sinsp_table_wrapper(sinsp_plugin* p, libsinsp::state::table<T>* t)
        : m_owner_plugin(p), m_key_type(typeinfo_to_state_type(t->key_info())),
		  m_table(t), m_field_list(), m_table_plugin_owner(nullptr), m_table_plugin_input(nullptr)
	{
		// note: if the we're wrapping a plugin-implemented table under the hood,
		// we just use the plugin-provided vtables right away instead of
		// going through the C++ wrapper. This is both faster and safer, also
		// because the current C++ wrapper for plugin-defined tables is just
		// a non-functional stub used only for complying to the registry interfaces.
		auto pt = dynamic_cast<plugin_table_wrapper<T>*>(t);
		if (pt)
		{
			m_table_plugin_owner = pt->m_owner;
			m_table_plugin_input = pt->m_input.get();
		}
	}

    sinsp_table_wrapper() = delete;
    sinsp_table_wrapper(sinsp_table_wrapper&&) = default;
    sinsp_table_wrapper& operator = (sinsp_table_wrapper&&) = default;
    sinsp_table_wrapper(const sinsp_table_wrapper& s) = delete;
    sinsp_table_wrapper& operator = (const sinsp_table_wrapper& s) = delete;
	virtual ~sinsp_table_wrapper()
	{
		for (auto& acc : m_field_accessors)
		{
			#define _X(_type, _dtype) \
			{ \
				if (acc.second.dynamic) \
				{ \
					delete static_cast<libsinsp::state::dynamic_struct::field_accessor<_type>*>(acc.second.accessor); \
				} \
				else \
				{ \
					delete static_cast<libsinsp::state::static_struct::field_accessor<_type>*>(acc.second.accessor); \
				} \
				break; \
			}
			std::string tmp;
			__CATCH_ERR_MSG(tmp, {
				__PLUGIN_STATETYPE_SWITCH(acc.second.data_type);
			});
			#undef _X
		}
	}

	sinsp_plugin* m_owner_plugin;
    ss_plugin_state_type m_key_type;
    libsinsp::state::base_table* m_table;
    std::vector<ss_plugin_table_fieldinfo> m_field_list;
    std::unordered_map<std::string, field_accessor_wrapper> m_field_accessors;
	const sinsp_plugin* m_table_plugin_owner;
	ss_plugin_table_input* m_table_plugin_input;

	static ss_plugin_table_fieldinfo* list_fields(ss_plugin_table_t* _t, uint32_t* nfields)
	{
		auto t = static_cast<sinsp_table_wrapper*>(_t);
	
		if (t->m_table_plugin_input)
		{
			auto pt = t->m_table_plugin_input->table;
			auto ret = t->m_table_plugin_input->fields.list_table_fields(pt, nfields);
			if (ret == NULL)
			{
				t->m_owner_plugin->m_last_owner_err = t->m_table_plugin_owner->get_last_error();
			}
			return ret;
		}

		__CATCH_ERR_MSG(t->m_owner_plugin->m_last_owner_err, {
			t->m_field_list.clear();
			for (auto& info : t->m_table->static_fields())
			{
				ss_plugin_table_fieldinfo i;
				i.name = info.second.name().c_str();
				i.field_type = typeinfo_to_state_type(info.second.info());
				i.read_only = info.second.readonly();
				t->m_field_list.push_back(i);
			}
			for (auto& info : t->m_table->dynamic_fields()->fields())
			{
				ss_plugin_table_fieldinfo i;
				i.name = info.second.name().c_str();
				i.field_type = typeinfo_to_state_type(info.second.info());
				i.read_only = false;
				t->m_field_list.push_back(i);
			}
			*nfields = t->m_field_list.size();
			return t->m_field_list.data();
		});
		return NULL;
	}

	static ss_plugin_table_field_t* get_field(ss_plugin_table_t* _t, const char* name, ss_plugin_state_type data_type)
	{
		auto t = static_cast<sinsp_table_wrapper*>(_t);

		if (t->m_table_plugin_input)
		{
			auto pt = t->m_table_plugin_input->table;
			auto ret = t->m_table_plugin_input->fields.get_table_field(pt, name, data_type);
			if (ret == NULL)
			{
				t->m_owner_plugin->m_last_owner_err = t->m_table_plugin_owner->get_last_error();
			}
			return ret;
		}

		libsinsp::state::static_struct::field_infos::const_iterator fixed_it;
		std::unordered_map<std::string, libsinsp::state::dynamic_struct::field_info>::const_iterator dyn_it;
		__CATCH_ERR_MSG(t->m_owner_plugin->m_last_owner_err, {
			auto it = t->m_field_accessors.find(name);
			if (it != t->m_field_accessors.end())
			{
				return static_cast<ss_plugin_table_field_t*>(&it->second);
			}

			fixed_it = t->m_table->static_fields().find(name);
			dyn_it = t->m_table->dynamic_fields()->fields().find(name);
			if (fixed_it != t->m_table->static_fields().end()
					&& dyn_it != t->m_table->dynamic_fields()->fields().end())
			{
				// todo(jasondellaluce): plugins are not aware of the difference
				// between static and dynamic fields. Do we want to enforce
				// this limitation in the sinsp tables implementation as well?
				throw sinsp_exception("field is defined as both static and dynamic: " + std::string(name));
			}
		});

		#define _X(_type, _dtype) \
		{ \
			auto acc = fixed_it->second.new_accessor<_type>(); \
			sinsp_table_wrapper::field_accessor_wrapper acc_wrap; \
			acc_wrap.dynamic = false; \
			acc_wrap.data_type = data_type; \
			acc_wrap.accessor = new libsinsp::state::static_struct::field_accessor<_type>(acc); \
			t->m_field_accessors[name] = acc_wrap; \
			return &t->m_field_accessors[name]; \
		}
		__CATCH_ERR_MSG(t->m_owner_plugin->m_last_owner_err, {
			if (fixed_it != t->m_table->static_fields().end())
			{
				if (data_type != typeinfo_to_state_type(fixed_it->second.info()))
				{
					throw sinsp_exception("incompatible data types for field: " + std::string(name));
				}	
				__PLUGIN_STATETYPE_SWITCH(data_type);
			}
		});
		#undef _X

		#define _X(_type, _dtype) \
		{ \
			auto acc = dyn_it->second.new_accessor<_type>(); \
			sinsp_table_wrapper::field_accessor_wrapper acc_wrap; \
			acc_wrap.dynamic = true; \
			acc_wrap.data_type = data_type; \
			acc_wrap.accessor = new libsinsp::state::dynamic_struct::field_accessor<_type>(acc); \
			t->m_field_accessors[name] = acc_wrap; \
			return &t->m_field_accessors[name]; \
		}
		__CATCH_ERR_MSG(t->m_owner_plugin->m_last_owner_err, {
			if (dyn_it != t->m_table->dynamic_fields()->fields().end())
			{
				if (data_type != typeinfo_to_state_type(dyn_it->second.info()))
				{
					throw sinsp_exception("incompatible data types for field: " + std::string(name));
				}
				__PLUGIN_STATETYPE_SWITCH(data_type);
			}
			throw sinsp_exception("undefined field '" + std::string(name) + "' in table '" + t->m_table->name() + "'");
		});
		#undef _X

		return NULL;
	}

	static ss_plugin_table_field_t* add_field(ss_plugin_table_t* _t, const char* name, ss_plugin_state_type data_type)
	{
		auto t = static_cast<sinsp_table_wrapper*>(_t);

		if (t->m_table_plugin_input)
		{
			auto pt = t->m_table_plugin_input->table;
			auto ret = t->m_table_plugin_input->fields.add_table_field(pt, name, data_type);
			if (ret == NULL)
			{
				t->m_owner_plugin->m_last_owner_err = t->m_table_plugin_owner->get_last_error();
			}
			return ret;
		}

		if (t->m_table->static_fields().find(name) != t->m_table->static_fields().end())
		{
			t->m_owner_plugin->m_last_owner_err = "can't add dynamic field already defined as static: " + std::string(name);
			return NULL;
		}
			
		#define _X(_type, _dtype) \
		{ \
			t->m_table->dynamic_fields()->add_field<_type>(name); \
			break; \
		}
		__CATCH_ERR_MSG(t->m_owner_plugin->m_last_owner_err, {
			__PLUGIN_STATETYPE_SWITCH(data_type);
			return get_field(_t, name, data_type);
		});
		#undef _X
		return NULL;
	}

	static const char* get_name(ss_plugin_table_t* _t)
	{
		auto t = static_cast<sinsp_table_wrapper*>(_t);

		if (t->m_table_plugin_input)
		{
			return t->m_table_plugin_input->name;
		}

		__CATCH_ERR_MSG(t->m_owner_plugin->m_last_owner_err, {
			return t->m_table->name().c_str();
		});
		return NULL;
	}

	static uint64_t get_size(ss_plugin_table_t* _t)
	{
		auto t = static_cast<sinsp_table_wrapper*>(_t);

		if (t->m_table_plugin_input)
		{
			auto pt = t->m_table_plugin_input->table;
			auto ret = t->m_table_plugin_input->reader.get_table_size(pt);
			if (ret == ((uint64_t) -1))
			{
				t->m_owner_plugin->m_last_owner_err = t->m_table_plugin_owner->get_last_error();
			}
			return ret;
		}

		__CATCH_ERR_MSG(t->m_owner_plugin->m_last_owner_err, {
			return t->m_table->entries_count();
		});
		return ((uint64_t) -1);
	}

	static ss_plugin_table_entry_t* get_entry(ss_plugin_table_t* _t, const ss_plugin_state_data* key)
	{
		auto t = static_cast<sinsp_table_wrapper*>(_t);

		if (t->m_table_plugin_input)
		{
			auto pt = t->m_table_plugin_input->table;
			auto ret = t->m_table_plugin_input->reader.get_table_entry(pt, key);
			if (ret == NULL)
			{
				t->m_owner_plugin->m_last_owner_err = t->m_table_plugin_owner->get_last_error();
			}
			return ret;
		}

		#define _X(_type, _dtype) \
		{ \
			auto tt = static_cast<libsinsp::state::table<_type>*>(t->m_table); \
			auto ret = tt->get_entry(key->_dtype); \
			if (ret != nullptr) \
			{ \
				return static_cast<ss_plugin_table_entry_t*>(ret.get()); \
			} \
			return NULL; \
		}
		__CATCH_ERR_MSG(t->m_owner_plugin->m_last_owner_err, {
			__PLUGIN_STATETYPE_SWITCH(t->m_key_type);
		});
		#undef _X
		return NULL;
	}

	static ss_plugin_rc read_entry_field(ss_plugin_table_t* _t, ss_plugin_table_entry_t* _e, const ss_plugin_table_field_t* f, ss_plugin_state_data* out);

	static ss_plugin_rc clear(ss_plugin_table_t* _t)
	{
		auto t = static_cast<sinsp_table_wrapper*>(_t);

		if (t->m_table_plugin_input)
		{
			auto pt = t->m_table_plugin_input->table;
			auto ret = t->m_table_plugin_input->writer.clear_table(pt);
			if (ret == SS_PLUGIN_FAILURE)
			{
				t->m_owner_plugin->m_last_owner_err = t->m_table_plugin_owner->get_last_error();
			}
			return ret;
		}

		__CATCH_ERR_MSG(t->m_owner_plugin->m_last_owner_err, {
			t->m_table->clear_entries();
			return SS_PLUGIN_SUCCESS;
		});
		return SS_PLUGIN_FAILURE;
	}

	static ss_plugin_rc erase_entry(ss_plugin_table_t* _t, const ss_plugin_state_data* key)
	{
		auto t = static_cast<sinsp_table_wrapper*>(_t);

		if (t->m_table_plugin_input)
		{
			auto pt = t->m_table_plugin_input->table;
			auto ret = t->m_table_plugin_input->writer.erase_table_entry(pt, key);
			if (ret == SS_PLUGIN_FAILURE)
			{
				t->m_owner_plugin->m_last_owner_err = t->m_table_plugin_owner->get_last_error();
			}
			return ret;
		}

		#define _X(_type, _dtype) \
		{ \
			if (static_cast<libsinsp::state::table<_type>*>(t->m_table)->erase_entry(key->_dtype)) \
			{ \
				return SS_PLUGIN_SUCCESS; \
			} \
			else \
			{ \
				t->m_owner_plugin->m_last_owner_err = "table entry not found"; \
				return SS_PLUGIN_FAILURE; \
			} \
		}
		__CATCH_ERR_MSG(t->m_owner_plugin->m_last_owner_err, {
			__PLUGIN_STATETYPE_SWITCH(t->m_key_type);
		});
		#undef _X
		return SS_PLUGIN_FAILURE;
	}

	static ss_plugin_table_entry_t* create_table_entry(ss_plugin_table_t* _t)
	{
		auto t = static_cast<sinsp_table_wrapper*>(_t);

		if (t->m_table_plugin_input)
		{
			auto pt = t->m_table_plugin_input->table;
			auto ret = t->m_table_plugin_input->writer.create_table_entry(pt);
			if (ret == NULL)
			{
				t->m_owner_plugin->m_last_owner_err = t->m_table_plugin_owner->get_last_error();
			}
			return ret;
		}

		#define _X(_type, _dtype) \
		{ \
			auto tt = static_cast<libsinsp::state::table<_type>*>(t->m_table); \
			auto ret = tt->new_entry().release(); \
			return static_cast<ss_plugin_table_entry_t*>(ret); \
		}
		__CATCH_ERR_MSG(t->m_owner_plugin->m_last_owner_err, {
			__PLUGIN_STATETYPE_SWITCH(t->m_key_type);
		});
		#undef _X
		return NULL;
	}

	static void destroy_table_entry(ss_plugin_table_t* _t, ss_plugin_table_entry_t* _e)
	{
		auto t = static_cast<sinsp_table_wrapper*>(_t);

		if (t->m_table_plugin_input)
		{
			auto pt = t->m_table_plugin_input->table;
			t->m_table_plugin_input->writer.destroy_table_entry(pt, _e);
		}

		#define _X(_type, _dtype) \
		{ \
			auto e = static_cast<libsinsp::state::table_entry*>(_e); \
			auto ptr = std::unique_ptr<libsinsp::state::table_entry>(e); \
			break; \
		}
		__CATCH_ERR_MSG(t->m_owner_plugin->m_last_owner_err, {
			__PLUGIN_STATETYPE_SWITCH(t->m_key_type);
		});
		#undef _X
	}

	static ss_plugin_table_entry_t* add_entry(ss_plugin_table_t* _t, const ss_plugin_state_data* key, ss_plugin_table_entry_t* _e)
	{
		auto t = static_cast<sinsp_table_wrapper*>(_t);

		if (t->m_table_plugin_input)
		{
			auto pt = t->m_table_plugin_input->table;
			auto ret = t->m_table_plugin_input->writer.add_table_entry(pt, key, _e);
			if (ret == NULL)
			{
				t->m_owner_plugin->m_last_owner_err = t->m_table_plugin_owner->get_last_error();
			}
			return ret;
		}

		#define _X(_type, _dtype) \
		{ \
			auto e = static_cast<libsinsp::state::table_entry*>(_e); \
			auto ptr = std::unique_ptr<libsinsp::state::table_entry>(e); \
			auto tt = static_cast<libsinsp::state::table<_type>*>(t->m_table); \
			auto ret = tt->add_entry(key->_dtype, std::move(ptr)).get(); \
			return static_cast<ss_plugin_table_entry_t*>(ret); \
		}
		__CATCH_ERR_MSG(t->m_owner_plugin->m_last_owner_err, {
			__PLUGIN_STATETYPE_SWITCH(t->m_key_type);
		});
		#undef _X
		return NULL;
	}

	static ss_plugin_rc write_entry_field(ss_plugin_table_t* _t, ss_plugin_table_entry_t* e, const ss_plugin_table_field_t* f, const ss_plugin_state_data* in);
};

ss_plugin_rc sinsp_table_wrapper::read_entry_field(ss_plugin_table_t* _t, ss_plugin_table_entry_t* _e, const ss_plugin_table_field_t* f, ss_plugin_state_data* out)
{
	auto t = static_cast<sinsp_table_wrapper*>(_t);

	if (t->m_table_plugin_input)
	{
		auto pt = t->m_table_plugin_input->table;
		auto ret = t->m_table_plugin_input->reader.read_entry_field(pt, _e, f, out);
		if (ret == SS_PLUGIN_FAILURE)
		{
			t->m_owner_plugin->m_last_owner_err = t->m_table_plugin_owner->get_last_error();
		}
		return ret;
	}

	auto a = static_cast<const sinsp_table_wrapper::field_accessor_wrapper*>(f);
	auto e = static_cast<libsinsp::state::table_entry*>(_e);
	#define _X(_type, _dtype) \
	{ \
		if (a->dynamic) \
		{ \
			auto aa = static_cast<libsinsp::state::dynamic_struct::field_accessor<_type>*>(a->accessor); \
			_type _v; \
			e->get_dynamic_field(*aa, _v); \
			convert_types(_v, out->_dtype); \
		} \
		else \
		{ \
			auto aa = static_cast<libsinsp::state::static_struct::field_accessor<_type>*>(a->accessor); \
			convert_types(e->get_static_field(*aa), out->_dtype); \
		} \
		return SS_PLUGIN_SUCCESS; \
	}
	__CATCH_ERR_MSG(t->m_owner_plugin->m_last_owner_err, {
		__PLUGIN_STATETYPE_SWITCH(a->data_type);
	});
	#undef _X
	return SS_PLUGIN_FAILURE;
}

ss_plugin_rc sinsp_table_wrapper::write_entry_field(ss_plugin_table_t* _t, ss_plugin_table_entry_t* _e, const ss_plugin_table_field_t* f, const ss_plugin_state_data* in)
{
	auto t = static_cast<sinsp_table_wrapper*>(_t);
	
	if (t->m_table_plugin_input)
	{
		auto pt = t->m_table_plugin_input->table;
		auto ret = t->m_table_plugin_input->writer.write_entry_field(pt, _e, f, in);
		if (ret == SS_PLUGIN_FAILURE)
		{
			t->m_owner_plugin->m_last_owner_err = t->m_table_plugin_owner->get_last_error();
		}
		return ret;
	}

	auto a = static_cast<const sinsp_table_wrapper::field_accessor_wrapper*>(f);
	auto e = static_cast<libsinsp::state::table_entry*>(_e);
	#define _X(_type, _dtype) \
	{ \
		if (a->dynamic) \
		{ \
			auto aa = static_cast<libsinsp::state::dynamic_struct::field_accessor<_type>*>(a->accessor); \
			e->set_dynamic_field<_type>(*aa, reinterpret_cast<const _type&>(in->_dtype)); \
		} \
		else \
		{ \
			auto aa = static_cast<libsinsp::state::static_struct::field_accessor<_type>*>(a->accessor); \
			e->set_static_field<_type>(*aa, in->_dtype); \
		} \
		return SS_PLUGIN_SUCCESS; \
	}
	__CATCH_ERR_MSG(t->m_owner_plugin->m_last_owner_err, {
		__PLUGIN_STATETYPE_SWITCH(a->data_type);
	});
	#undef _X
	return SS_PLUGIN_FAILURE;
}

// the following table api symbols act as dispatcher for the table API
// interface, which is implemented through the type ss_plugin_table_input.
// For sinsp-defined tables, the ss_plugin_table_input is a wrapper around
// the libsinsp::state::table interface. For plugin-defined tables, the
// ss_plugin_table_input is provided by the table-owner plugin itself.
static ss_plugin_table_fieldinfo* dispatch_list_fields(ss_plugin_table_t *_t, uint32_t *nfields)
{
	auto t = static_cast<ss_plugin_table_input*>(_t);
	return t->fields.list_table_fields(t->table, nfields);
}

static ss_plugin_table_field_t* dispatch_get_field(ss_plugin_table_t* _t, const char* name, ss_plugin_state_type data_type)
{
	auto t = static_cast<ss_plugin_table_input*>(_t);
	return t->fields.get_table_field(t->table, name, data_type);
}

static ss_plugin_table_field_t* dispatch_add_field(ss_plugin_table_t* _t, const char* name, ss_plugin_state_type data_type)
{
	auto t = static_cast<ss_plugin_table_input*>(_t);
	return t->fields.add_table_field(t->table, name, data_type);
}

static const char* dispatch_get_name(ss_plugin_table_t* _t)
{
	auto t = static_cast<ss_plugin_table_input*>(_t);
	return t->reader.get_table_name(t->table);
}

static uint64_t dispatch_get_size(ss_plugin_table_t* _t)
{
	auto t = static_cast<ss_plugin_table_input*>(_t);
	return t->reader.get_table_size(t->table);
}

static ss_plugin_table_entry_t* dispatch_get_entry(ss_plugin_table_t* _t, const ss_plugin_state_data* key)
{
	auto t = static_cast<ss_plugin_table_input*>(_t);
	return t->reader.get_table_entry(t->table, key);
}

static ss_plugin_rc dispatch_read_entry_field(ss_plugin_table_t* _t, ss_plugin_table_entry_t* e, const ss_plugin_table_field_t* f, ss_plugin_state_data* out)
{
	auto t = static_cast<ss_plugin_table_input*>(_t);
	return t->reader.read_entry_field(t->table, e, f, out);
}

static ss_plugin_rc dispatch_clear(ss_plugin_table_t* _t)
{
	auto t = static_cast<ss_plugin_table_input*>(_t);
	return t->writer.clear_table(t->table);
}

static ss_plugin_rc dispatch_erase_entry(ss_plugin_table_t* _t, const ss_plugin_state_data* key)
{
	auto t = static_cast<ss_plugin_table_input*>(_t);
	return t->writer.erase_table_entry(t->table, key);
}

static ss_plugin_table_entry_t* dispatch_create_table_entry(ss_plugin_table_t* _t)
{
	auto t = static_cast<ss_plugin_table_input*>(_t);
	return t->writer.create_table_entry(t->table);
}

static void dispatch_destroy_table_entry(ss_plugin_table_t* _t, ss_plugin_table_entry_t* e)
{
	auto t = static_cast<ss_plugin_table_input*>(_t);
	return t->writer.destroy_table_entry(t->table, e);
}

static ss_plugin_table_entry_t* dispatch_add_entry(ss_plugin_table_t* _t, const ss_plugin_state_data* key, ss_plugin_table_entry_t* entry)
{
	auto t = static_cast<ss_plugin_table_input*>(_t);
	return t->writer.add_table_entry(t->table, key, entry);
}

static ss_plugin_rc dispatch_write_entry_field(ss_plugin_table_t* _t, ss_plugin_table_entry_t* e, const ss_plugin_table_field_t* f, const ss_plugin_state_data* in)
{
	auto t = static_cast<ss_plugin_table_input*>(_t);
	return t->writer.write_entry_field(t->table, e, f, in);
}

void sinsp_plugin::table_field_api(ss_plugin_table_fields_vtable& out)
{
	out.list_table_fields = dispatch_list_fields;
	out.add_table_field = dispatch_add_field;
	out.get_table_field = dispatch_get_field;
}

void sinsp_plugin::table_read_api(ss_plugin_table_reader_vtable& out)
{
	out.get_table_name = dispatch_get_name;
	out.get_table_size = dispatch_get_size;
	out.get_table_entry = dispatch_get_entry;
	out.read_entry_field = dispatch_read_entry_field;
}

void sinsp_plugin::table_write_api(ss_plugin_table_writer_vtable& out)
{
	out.clear_table = dispatch_clear;
	out.erase_table_entry = dispatch_erase_entry;
	out.create_table_entry = dispatch_create_table_entry;
	out.destroy_table_entry = dispatch_destroy_table_entry;
	out.add_table_entry = dispatch_add_entry;
	out.write_entry_field = dispatch_write_entry_field;
}

ss_plugin_table_info* sinsp_plugin::table_api_list_tables(ss_plugin_owner_t* o, uint32_t* ntables)
{
	auto p = static_cast<sinsp_plugin*>(o);
	__CATCH_ERR_MSG(p->m_last_owner_err, {
		*ntables = 0;
		p->m_table_infos.clear();
		for (const auto &d : p->m_table_registry->tables())
		{
			ss_plugin_table_info info;
			info.name = d.second->name().c_str();
			info.key_type = typeinfo_to_state_type(d.second->key_info());
			p->m_table_infos.push_back(info);
		}
		*ntables = p->m_table_infos.size();
		return p->m_table_infos.data();
	});
	return NULL;
}

void sinsp_plugin::table_input_deleter::operator()(ss_plugin_table_input* in)
{
    delete static_cast<sinsp_table_wrapper*>(in->table);
    delete in;
}

ss_plugin_table_t* sinsp_plugin::table_api_get_table(ss_plugin_owner_t *o, const char *name, ss_plugin_state_type key_type)
{
	auto p = static_cast<sinsp_plugin*>(o);

	// if a plugin is accessing a plugin-owned table, we return it as-is
	// instead of wrapping it. This is both more performant and safer from
	// a memory ownership perspective, because the other plugin is the actual
	// total owner of the table's memory. Note, even though dynamic_cast is
	// generally quite expensive, the "get_table" primitive can only be
	// used during plugin initialization, so it's not in the hot path.
	#define _X(_type, _dtype) \
	{ \
		auto t = p->m_table_registry->get_table<_type>(name); \
		if (!t) \
		{ \
			return NULL; \
		} \
		accessed_table_t res(new ss_plugin_table_input(), table_input_deleter()); \
		auto state = new sinsp_table_wrapper(p, t); \
		res->table = static_cast<ss_plugin_table_t*>(state); \
		res->name = state->m_table->name().c_str(); \
		res->key_type = state->m_key_type; \
		res->fields.list_table_fields = sinsp_table_wrapper::list_fields; \
		res->fields.add_table_field = sinsp_table_wrapper::add_field; \
		res->fields.get_table_field = sinsp_table_wrapper::get_field; \
		res->reader.get_table_name = sinsp_table_wrapper::get_name; \
		res->reader.get_table_size = sinsp_table_wrapper::get_size; \
		res->reader.get_table_entry = sinsp_table_wrapper::get_entry; \
		res->reader.read_entry_field = sinsp_table_wrapper::read_entry_field; \
		res->writer.clear_table = sinsp_table_wrapper::clear; \
		res->writer.erase_table_entry = sinsp_table_wrapper::erase_entry; \
		res->writer.create_table_entry = sinsp_table_wrapper::create_table_entry; \
		res->writer.destroy_table_entry = sinsp_table_wrapper::destroy_table_entry; \
		res->writer.add_table_entry = sinsp_table_wrapper::add_entry; \
		res->writer.write_entry_field = sinsp_table_wrapper::write_entry_field; \
		p->m_accessed_tables[name] = std::move(res); \
		return p->m_accessed_tables[name].get(); \
	};
	__CATCH_ERR_MSG(p->m_last_owner_err, {
		const auto& tables = p->m_accessed_tables;
		auto it = tables.find(name);
		if (it == tables.end())
		{
			__PLUGIN_STATETYPE_SWITCH(key_type);
		}
		return it->second.get();
	});
	#undef _X
	return NULL;
}

ss_plugin_rc sinsp_plugin::table_api_add_table(ss_plugin_owner_t *o, const ss_plugin_table_input* in)
{
	auto p = static_cast<sinsp_plugin*>(o);
	#define _X(_type, _dtype) \
	{ \
		auto t = new plugin_table_wrapper<_type>(p, in); \
		p->m_table_registry->add_table(t); \
		p->m_owned_tables[in->name] = sinsp_plugin::owned_table_t(t); \
		break; \
	}
	__CATCH_ERR_MSG(p->m_last_owner_err, {
		__PLUGIN_STATETYPE_SWITCH(in->key_type);
		return SS_PLUGIN_SUCCESS;
	});
	#undef _X
	return SS_PLUGIN_FAILURE;
}
