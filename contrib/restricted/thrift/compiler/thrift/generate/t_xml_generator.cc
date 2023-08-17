/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements. See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership. The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License. You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied. See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include <fstream>
#include <iostream>
#include <sstream>
#include <limits>

#include <stdlib.h>
#include <sys/stat.h>
#include <sstream>

#include "thrift/platform.h"
#include "thrift/generate/t_generator.h"

using std::map;
using std::ofstream;
using std::ostream;
using std::ostringstream;
using std::string;
using std::stringstream;
using std::vector;
using std::stack;
using std::set;

static const string endl = "\n";
static const string quot = "\"";

static const string default_ns_prefix = "http://thrift.apache.org/xml/ns/";

/**
 * This generator creates an XML model of the parsed IDL tree, and is designed
 * to make it easy to use this file as the input for other template engines,
 * such as XSLT.  To this end, the generated XML is slightly more verbose than
 * you might expect... for example, references to "id" types (such as structs,
 * unions, etc) always specify the name of the IDL document, even if the type
 * is defined in the same document as the reference.
 */
class t_xml_generator : public t_generator {
public:
  t_xml_generator( t_program* program,
                   const std::map<std::string, std::string>& parsed_options,
                   const std::string& option_string)
    : t_generator(program) {
    (void)option_string;
    std::map<std::string, std::string>::const_iterator iter;

    should_merge_includes_ = false;
    should_use_default_ns_ = true;
    should_use_namespaces_ = true;
    for( iter = parsed_options.begin(); iter != parsed_options.end(); ++iter) {
      if( iter->first.compare("merge") == 0) {
        should_merge_includes_ = true;
      } else if( iter->first.compare("no_default_ns") == 0) {
        should_use_default_ns_ = false;
      } else if( iter->first.compare("no_namespaces") == 0) {
        should_use_namespaces_ = false;
      } else {
        throw "unknown option xml:" + iter->first;
      }
    }

    out_dir_base_ = "gen-xml";
  }

  virtual ~t_xml_generator() {}

  void init_generator();
  void close_generator();
  void generate_program();

  void iterate_program(t_program* program);
  void generate_typedef(t_typedef* ttypedef);
  void generate_enum(t_enum* tenum);
  void generate_function(t_function* tfunc);
  void generate_field(t_field* field);

  void generate_service(t_service* tservice);
  void generate_struct(t_struct* tstruct);

  void generate_annotations(std::map<std::string, std::string> annotations);

private:
  bool should_merge_includes_;
  bool should_use_default_ns_;
  bool should_use_namespaces_;

  ofstream_with_content_based_conditional_update f_xml_;

  std::set<string> programs_;
  std::stack<string> elements_;
  bool top_element_is_empty;
  bool top_element_is_open;

  string target_namespace(t_program* program);
  void write_element_start(const string name);
  void close_top_element();
  void write_element_end();
  void write_attribute(string key, string val);
  void write_int_attribute(string key, int val);
  string escape_xml_string(const string& input);

  void write_xml_comment(string msg);

  void write_type(t_type* ttype);
  void write_doc(t_doc* tdoc);

  template <typename T>
  string number_to_string(T t) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out.precision(std::numeric_limits<T>::digits10);
    out << t;
    return out.str();
  }

  template <typename T>
  void write_number(T n) {
    f_xml_ << number_to_string(n);
  }

  template <typename T>
  void write_element_number(string name, T n) {
    write_element_string(name, number_to_string(n));
  }

  string get_type_name(t_type* ttype);

  void generate_constant(t_const* con);

  void write_element_string(string name, string value);
  void write_value(t_type* tvalue);
  void write_const_value(t_const_value* value);
  virtual std::string xml_autogen_comment() {
    return std::string("\n") + " * Autogenerated by Thrift Compiler (" + THRIFT_VERSION + ")\n"
           + " *\n" + " * DO NOT EDIT UNLESS YOU ARE SURE THAT YOU KNOW WHAT YOU ARE DOING\n";
  }
};

void t_xml_generator::init_generator() {
  MKDIR(get_out_dir().c_str());

  string f_xml_name = get_out_dir() + program_->get_name() + ".xml";
  f_xml_.open(f_xml_name.c_str());

  top_element_is_open = false;
}

string t_xml_generator::target_namespace(t_program* program) {
  std::map<std::string, std::string> map;
  std::map<std::string, std::string>::iterator iter;
  map = program->get_namespace_annotations("xml");
  if ((iter = map.find("targetNamespace")) != map.end()) {
    return iter->second;
  }
  map = program->get_namespaces();
  if ((iter = map.find("xml")) != map.end()) {
    return default_ns_prefix + iter->second;
  }
  map = program->get_namespace_annotations("*");
  if ((iter = map.find("xml.targetNamespace")) != map.end()) {
    return iter->second;
  }
  map = program->get_namespaces();
  if ((iter = map.find("*")) != map.end()) {
    return default_ns_prefix + iter->second;
  }
  return default_ns_prefix + program->get_name();
}

void t_xml_generator::write_xml_comment(string msg) {
  close_top_element();
  // TODO: indent any EOLs that may occur with msg
  // TODO: proper msg escaping needed?
  f_xml_ << indent() << "<!-- " << msg << " -->"  << endl;
  top_element_is_empty = false;
}

void t_xml_generator::close_top_element() {
  if( top_element_is_open) {
    top_element_is_open = false;
    if (elements_.size() > 0 && top_element_is_empty) {
      f_xml_ << ">" << endl;
    }
  }
}

void t_xml_generator::write_element_start(string name) {
  if (should_use_namespaces_ && !should_use_default_ns_) {
    name = "idl:" + name;
  }
  close_top_element();
  f_xml_ << indent() << "<" << name;
  elements_.push(name);
  top_element_is_empty = true;
  top_element_is_open = true;
  indent_up();
}

void t_xml_generator::write_element_end() {
  indent_down();
  if (top_element_is_empty && top_element_is_open) {
    f_xml_ << " />" << endl;
  } else {
    f_xml_ << indent() << "</" << elements_.top() << ">" << endl;
  }
  top_element_is_empty = false;
  elements_.pop();
}

void t_xml_generator::write_attribute(string key, string val) {
  f_xml_ << " " << key << "=\"" << escape_xml_string(val) << "\"";
}

void t_xml_generator::write_int_attribute(string key, int val) {
  write_attribute(key, number_to_string(val));
}

void t_xml_generator::write_element_string(string name, string val) {
  if (should_use_namespaces_ && !should_use_default_ns_) {
    name = "idl:" + name;
  }
  close_top_element();
  top_element_is_empty = false;
  f_xml_ << indent()
    << "<" << name << ">" << escape_xml_string(val) << "</" << name << ">"
    << endl;
}

string t_xml_generator::escape_xml_string(const string& input) {
  std::ostringstream ss;
  for (std::string::const_iterator iter = input.begin(); iter != input.end(); iter++) {
    switch (*iter) {
    case '&':
      ss << "&amp;";
      break;
    case '"':
      ss << "&quot;";
      break;
    case '\'':
      ss << "&apos;";
      break;
    case '<':
      ss << "&lt;";
      break;
    case '>':
      ss << "&gt;";
      break;
    default:
      ss << *iter;
      break;
    }
  }
  return ss.str();
}

void t_xml_generator::close_generator() {
  f_xml_.close();
}

void t_xml_generator::generate_program() {

  init_generator();

  write_element_start("idl");
  if (should_use_namespaces_) {
    if (should_use_default_ns_) {
      write_attribute("xmlns", "http://thrift.apache.org/xml/idl");
    }
    write_attribute("xmlns:idl", "http://thrift.apache.org/xml/idl");
  }

  write_xml_comment( xml_autogen_comment());

  iterate_program(program_);

  write_element_end();

  close_generator();

}

void t_xml_generator::iterate_program(t_program* program) {

  write_element_start("document");
  write_attribute("name", program->get_name());
  if (should_use_namespaces_) {
    const string targetNamespace = target_namespace(program);
    write_attribute("targetNamespace", targetNamespace);
    write_attribute("xmlns:" + program->get_name(), targetNamespace);
  }
  write_doc(program);

  const vector<t_program*> includes = program->get_includes();
  vector<t_program*>::const_iterator inc_it;
  for (inc_it = includes.begin(); inc_it != includes.end(); ++inc_it) {
    write_element_start("include");
    write_attribute("name", (*inc_it)->get_name());
    write_element_end();
  }

  const map<string, string>& namespaces = program->get_namespaces();
  map<string, string>::const_iterator ns_it;
  for (ns_it = namespaces.begin(); ns_it != namespaces.end(); ++ns_it) {
    write_element_start("namespace");
    write_attribute("name", ns_it->first);
    write_attribute("value", ns_it->second);
    generate_annotations(program->get_namespace_annotations(ns_it->first));
    write_element_end();
  }

  // TODO: can constants have annotations?
  vector<t_const*> consts = program->get_consts();
  vector<t_const*>::iterator c_iter;
  for (c_iter = consts.begin(); c_iter != consts.end(); ++c_iter) {
    generate_constant(*c_iter);
  }

  vector<t_typedef*> typedefs = program->get_typedefs();
  vector<t_typedef*>::iterator td_iter;
  for (td_iter = typedefs.begin(); td_iter != typedefs.end(); ++td_iter) {
    generate_typedef(*td_iter);
  }

  vector<t_enum*> enums = program->get_enums();
  vector<t_enum*>::iterator en_iter;
  for (en_iter = enums.begin(); en_iter != enums.end(); ++en_iter) {
    generate_enum(*en_iter);
  }

  vector<t_struct*> objects = program->get_objects();
  vector<t_struct*>::iterator o_iter;
  for (o_iter = objects.begin(); o_iter != objects.end(); ++o_iter) {
    if ((*o_iter)->is_xception()) {
      generate_xception(*o_iter);
    } else {
      generate_struct(*o_iter);
    }
  }

  vector<t_service*> services = program->get_services();
  vector<t_service*>::iterator sv_iter;
  for (sv_iter = services.begin(); sv_iter != services.end(); ++sv_iter) {
    generate_service(*sv_iter);
  }

  write_element_end();

  if (should_merge_includes_) {
    programs_.insert(program->get_name());
    const vector<t_program*> programs = program->get_includes();
    vector<t_program*>::const_iterator prog_it;
    for (prog_it = programs.begin(); prog_it != programs.end(); ++prog_it) {
      if (!programs_.count((*prog_it)->get_name())) {
        iterate_program(*prog_it);
      }
    }
  }

}

void t_xml_generator::generate_typedef(t_typedef* ttypedef) {
  write_element_start("typedef");
  write_attribute("name", ttypedef->get_name());
  write_doc(ttypedef);
  write_type(ttypedef->get_true_type());
  generate_annotations(ttypedef->annotations_);
  write_element_end();
  return;
}

void t_xml_generator::write_type(t_type* ttype) {
  const string type = get_type_name(ttype);
  write_attribute("type", type);
  if (type == "id") {
    write_attribute("type-module", ttype->get_program()->get_name());
    write_attribute("type-id", ttype->get_name());
  } else if (type == "list") {
    t_type* etype = ((t_list*)ttype)->get_elem_type();
    write_element_start("elemType");
    write_type(etype);
    write_element_end();
  } else if (type == "set") {
    t_type* etype = ((t_set*)ttype)->get_elem_type();
    write_element_start("elemType");
    write_type(etype);
    write_element_end();
  } else if (type == "map") {
    t_type* ktype = ((t_map*)ttype)->get_key_type();
    write_element_start("keyType");
    write_type(ktype);
    write_element_end();
    t_type* vtype = ((t_map*)ttype)->get_val_type();
    write_element_start("valueType");
    write_type(vtype);
    write_element_end();
  }
}

void t_xml_generator::write_doc(t_doc* tdoc) {
  if (tdoc->has_doc()) {
    string doc = tdoc->get_doc();
    // for some reason there always seems to be a trailing newline on doc
    // comments; loop below naively tries to strip off trailing cr/lf
    int n = 0;
    for (string::reverse_iterator i = doc.rbegin(); i != doc.rend(); i++,n++) {
      if (*i != '\n' || *i == '\r') {
        if (n > 0) {
          doc.erase(doc.length() - n);
        }
        break;
      }
    }
    write_attribute("doc", doc);
  }
}

void t_xml_generator::generate_annotations(
    std::map<std::string, std::string> annotations) {
  std::map<std::string, std::string>::iterator iter;
  for (iter = annotations.begin(); iter != annotations.end(); ++iter) {
    write_element_start("annotation");
    write_attribute("key", iter->first);
    write_attribute("value", iter->second);
    write_element_end();
  }
}

void t_xml_generator::generate_constant(t_const* con) {
  write_element_start("const");
  write_attribute("name", con->get_name());
  write_doc(con);
  write_type(con->get_type());
  write_const_value(con->get_value());
  write_element_end();
}

void t_xml_generator::write_const_value(t_const_value* value) {

  switch (value->get_type()) {

  case t_const_value::CV_IDENTIFIER:
  case t_const_value::CV_INTEGER:
    write_element_number("int", value->get_integer());
    break;

  case t_const_value::CV_DOUBLE:
    write_element_number("double", value->get_double());
    break;

  case t_const_value::CV_STRING:
    write_element_string("string", value->get_string());
    break;

  case t_const_value::CV_LIST: {
    write_element_start("list");
    std::vector<t_const_value*> list = value->get_list();
    std::vector<t_const_value*>::iterator lit;
    for (lit = list.begin(); lit != list.end(); ++lit) {
      write_element_start("entry");
      write_const_value(*lit);
      write_element_end();
    }
    write_element_end();
    break;
  }

  case t_const_value::CV_MAP: {
    write_element_start("map");
    std::map<t_const_value*, t_const_value*, t_const_value::value_compare> map = value->get_map();
    std::map<t_const_value*, t_const_value*, t_const_value::value_compare>::iterator mit;
    for (mit = map.begin(); mit != map.end(); ++mit) {
      write_element_start("entry");
      write_element_start("key");
      write_const_value(mit->first);
      write_element_end();
      write_element_start("value");
      write_const_value(mit->second);
      write_element_end();
      write_element_end();
    }
    write_element_end();
    break;
  }

  default:
    indent_up();
    f_xml_ << indent() << "<null />" << endl;
    indent_down();
    break;
  }

}

void t_xml_generator::generate_enum(t_enum* tenum) {

  write_element_start("enum");
  write_attribute("name", tenum->get_name());
  write_doc(tenum);

  vector<t_enum_value*> values = tenum->get_constants();
  vector<t_enum_value*>::iterator val_iter;
  for (val_iter = values.begin(); val_iter != values.end(); ++val_iter) {
    t_enum_value* val = (*val_iter);
    write_element_start("member");
    write_attribute("name", val->get_name());
    write_int_attribute("value", val->get_value());
    write_doc(val);
    generate_annotations(val->annotations_);
    write_element_end();
  }

  generate_annotations(tenum->annotations_);

  write_element_end();

}

void t_xml_generator::generate_struct(t_struct* tstruct) {

  string tagname = "struct";
  if (tstruct->is_union()) {
    tagname = "union";
  } else if (tstruct->is_xception()) {
    tagname = "exception";
  }

  write_element_start(tagname);
  write_attribute("name", tstruct->get_name());
  write_doc(tstruct);
  vector<t_field*> members = tstruct->get_members();
  vector<t_field*>::iterator mem_iter;
  for (mem_iter = members.begin(); mem_iter != members.end(); mem_iter++) {
    write_element_start("field");
    generate_field(*mem_iter);
    write_element_end();
  }

  generate_annotations(tstruct->annotations_);

  write_element_end();

}

void t_xml_generator::generate_field(t_field* field) {
  write_attribute("name", field->get_name());
  write_int_attribute("field-id", field->get_key());
  write_doc(field);
  string requiredness;
  switch (field->get_req()) {
  case t_field::T_REQUIRED:
    requiredness = "required";
    break;
  case t_field::T_OPTIONAL:
    requiredness = "optional";
    break;
  default:
    requiredness = "";
    break;
  }
  if (requiredness != "") {
    write_attribute("required", requiredness);
  }
  write_type(field->get_type());
  if (field->get_value()) {
    write_element_start("default");
    write_const_value(field->get_value());
    write_element_end();
  }
  generate_annotations(field->annotations_);
}

void t_xml_generator::generate_service(t_service* tservice) {

  write_element_start("service");
  write_attribute("name", tservice->get_name());

  if (should_use_namespaces_) {
    string prog_ns = target_namespace(tservice->get_program());
    if (*prog_ns.rbegin() != '/') {
      prog_ns.push_back('/');
    }
    const string tns = prog_ns + tservice->get_name();
    write_attribute("targetNamespace", tns);
    write_attribute("xmlns:tns", tns);
  }

  if (tservice->get_extends()) {
    const t_service* extends = tservice->get_extends();
    write_attribute("parent-module", extends->get_program()->get_name());
    write_attribute("parent-id", extends->get_name());
  }

  write_doc(tservice);

  vector<t_function*> functions = tservice->get_functions();
  vector<t_function*>::iterator fn_iter = functions.begin();
  for (; fn_iter != functions.end(); fn_iter++) {
    generate_function(*fn_iter);
  }

  generate_annotations(tservice->annotations_);

  write_element_end();

}

void t_xml_generator::generate_function(t_function* tfunc) {

  write_element_start("method");

  write_attribute("name", tfunc->get_name());
  if (tfunc->is_oneway()) {
    write_attribute("oneway", "true");
  }

  write_doc(tfunc);

  write_element_start("returns");
  write_type(tfunc->get_returntype());
  write_element_end();

  vector<t_field*> members = tfunc->get_arglist()->get_members();
  vector<t_field*>::iterator mem_iter = members.begin();
  for (; mem_iter != members.end(); mem_iter++) {
    write_element_start("arg");
    generate_field(*mem_iter);
    write_element_end();
  }

  vector<t_field*> excepts = tfunc->get_xceptions()->get_members();
  vector<t_field*>::iterator ex_iter = excepts.begin();
  for (; ex_iter != excepts.end(); ex_iter++) {
    write_element_start("throws");
    generate_field(*ex_iter);
    write_element_end();
  }

  generate_annotations(tfunc->annotations_);

  write_element_end();

}

string t_xml_generator::get_type_name(t_type* ttype) {
  if (ttype->is_list()) {
    return "list";
  }
  if (ttype->is_set()) {
    return "set";
  }
  if (ttype->is_map()) {
    return "map";
  }
  if ((ttype->is_enum()    )||
      (ttype->is_struct()  )||
      (ttype->is_typedef() )||
      (ttype->is_xception())){
    return "id";
  }
  if (ttype->is_base_type()) {
    t_base_type* tbasetype = (t_base_type*)ttype;
    if (tbasetype->is_binary() ) {
      return "binary";
    }
    return t_base_type::t_base_name(tbasetype->get_base());
  }
  return "(unknown)";
}

THRIFT_REGISTER_GENERATOR(
  xml,
  "XML",
  "    merge:           Generate output with included files merged\n"
  "    no_default_ns:   Omit default xmlns and add idl: prefix to all elements\n"
  "    no_namespaces:   Do not add namespace definitions to the XML model\n")
