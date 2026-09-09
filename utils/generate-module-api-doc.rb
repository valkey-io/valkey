#!/usr/bin/env ruby
# coding: utf-8
# gendoc.rb -- Converts the top-comments inside module.c to modules API
#              reference documentation in markdown format.

# Convert the C comment to markdown
def markdown(s)
    s = s.gsub(/\*\/$/,"")
    s = s.gsub(/^ ?\* ?/,"")
    s = s.gsub(/^\/\*\*? ?/,"")
    s.chop! while s[-1] == "\n" || s[-1] == " "
    lines = s.split("\n")
    newlines = []
    # Fix some markdown
    lines.each{|l|
        # Rewrite VM_Xyz() to ValkeyModule_Xyz().
        l = l.gsub(/(?<![A-Z_])VM_(?=[A-Z])/, 'ValkeyModule_')
        # Fix more markdown, except in code blocks indented by 4 spaces, which we
        # don't want to mess with.
        if not l.start_with?('    ')
            # Add backquotes around ValkeyModule functions and type where missing.
            l = l.gsub(/(?<!`)ValkeyModule[A-z]+(?:\*?\(\))?/){|x| "`#{x}`"}
            # Add backquotes around c functions like malloc() where missing.
            l = l.gsub(/(?<![`A-z.])[a-z_]+\(\)/, '`\0`')
            # Add backquotes around macro and var names containing underscores.
            l = l.gsub(/(?<![`A-z\*])[A-Za-z]+_[A-Za-z0-9_]+/){|x| "`#{x}`"}
            # Link URLs preceded by space or newline (not already linked)
            l = l.gsub(/(^| )(https?:\/\/[A-Za-z0-9_\/\.\?=&+\#\-]+[A-Za-z0-9\/])/,
                       '\1[\2](\2)')
            # Replace double-dash with unicode ndash
            l = l.gsub(/ -- /, ' – ')
        end
        # Link function names to their definition within the page
        l = l.gsub(/`(ValkeyModule_[A-z0-9]+)[()]*`/) {|x|
            $index[$1] ? "[#{x}](\##{$1})" : x
        }
        newlines << l
    }
    return newlines.join("\n")
end

# Linebreak a prototype longer than 80 characters on the commas, but only
# between balanced parentheses so that we don't linebreak args which are
# function pointers, and then aligning each arg under each other.
def linebreak_proto(proto, indent)
    if proto.bytesize <= 80
        return proto
    end
    
    # Find the opening parenthesis for parameters
    paren_pos = proto.index("(")
    return proto if paren_pos.nil?
    
    # Split the prototype into parts: return_type function_name(params);
    before_params = proto[0..paren_pos]
    params_and_end = proto[paren_pos+1..-1]
    
    # Find the closing parenthesis (handling nested parentheses)
    bracket_count = 0
    close_paren_pos = nil
    params_and_end.each_char.with_index do |char, idx|
        if char == '('
            bracket_count += 1
        elsif char == ')'
            if bracket_count == 0
                close_paren_pos = idx
                break
            else
                bracket_count -= 1
            end
        end
    end
    
    return proto if close_paren_pos.nil?
    
    params = params_and_end[0...close_paren_pos]
    after_params = params_and_end[close_paren_pos..-1]
    
    # Split parameters on commas, but respect nested parentheses
    param_parts = []
    current_param = ""
    bracket_balance = 0
    
    params.each_char do |char|
        if char == '('
            bracket_balance += 1
            current_param += char
        elsif char == ')'
            bracket_balance -= 1
            current_param += char
        elsif char == ',' && bracket_balance == 0
            param_parts << current_param.strip
            current_param = ""
        else
            current_param += char
        end
    end
    
    # Add the last parameter
    param_parts << current_param.strip if !current_param.strip.empty?
    
    # If only one parameter or very short, don't break
    if param_parts.length <= 1
        return proto
    end
    
    # Build the formatted result
    align_pos = before_params.length
    align = " " * align_pos
    result = before_params + param_parts.shift
    
    param_parts.each do |part|
        result += ",\n" + indent + align + part
    end
    
    result += after_params
    return result
end

# Given the source code array and the index at which an exported symbol was
# detected, extracts and outputs the documentation.
def docufy(src,i)
    m = /VM_[A-z0-9]+/.match(src[i])
    shortname = m[0].sub("VM_","")
    name = "ValkeyModule_" ++ shortname
    
    # Build the complete function prototype by reading until we find the opening brace
    proto_lines = []
    j = i
    while j < src.length
        line = src[j].rstrip
        if line.include?("{")
            # Include the part before the brace
            line = line.sub(/\s*\{.*$/, "")
            proto_lines << line unless line.strip.empty?
            break
        else
            proto_lines << line
        end
        j += 1
    end
    
    # Join all lines and clean up
    proto = proto_lines.join(" ")
    
    # Remove extra whitespace and normalize
    proto = proto.gsub(/\s+/, " ").strip
    
    # Replace VM_ with ValkeyModule_
    proto = proto.sub("VM_","ValkeyModule_")
    
    # Add semicolon if not present
    proto += ";" unless proto.end_with?(";")
    
    # Apply line breaking
    proto = linebreak_proto(proto, "    ");
    
    # Add a link target with the function name. (We don't trust the exact id of
    # the generated one, which depends on the Markdown implementation.)
    puts "<span id=\"#{name}\"></span>\n\n"
    puts "### `#{name}`\n\n"
    puts "    #{proto}\n\n"
    puts "**Available since:** #{$since[shortname] or "unreleased"}\n\n"
    comment = ""
    while true
        i = i-1
        comment = src[i]+comment
        break if src[i] =~ /\/\*/
    end
    comment = markdown(comment)
    puts comment+"\n\n"
end

# Print a comment from line until */ is found, as markdown.
def section_doc(src, i)
    name = get_section_heading(src, i)
    comment = "<span id=\"#{section_name_to_id(name)}\"></span>\n\n"
    while true
         # append line, except if it's a horizontal divider
        comment = comment + src[i] if src[i] !~ /^[\/ ]?\*{1,2} ?-{50,}/
        break if src[i] =~ /\*\//
        i = i+1
    end
    comment = markdown(comment)
    puts comment+"\n\n"
end

# generates an id suitable for links within the page
def section_name_to_id(name)
    return "section-" +
           name.strip.downcase.gsub(/[^a-z0-9]+/, '-').gsub(/^-+|-+$/, '')
end

# Returns the name of the first section heading in the comment block for which
# is_section_doc(src, i) is true
def get_section_heading(src, i)
    if src[i] =~ /^\/\*\*? \#+ *(.*)/
        heading = $1
    elsif src[i+1] =~ /^ ?\* \#+ *(.*)/
        heading = $1
    end
    return heading.gsub(' -- ', ' – ')
end

# Returns true if the line is the start of a generic documentation section. Such
# section must start with the # symbol, i.e. a markdown heading, on the first or
# the second line.
def is_section_doc(src, i)
    return src[i] =~ /^\/\*\*? \#/ ||
           (src[i] =~ /^\/\*/ && src[i+1] =~ /^ ?\* \#/)
end

def is_func_line(src, i)
  line = src[i]
  return line =~ /VM_/ &&
         line[0] != ' ' && line[0] != '#' && line[0] != '/' &&
         src[i-1] =~ /\*\//
end

puts "---\n"
puts "title: \"Modules API reference\"\n"
puts "linkTitle: \"API reference\"\n"
puts "description: >\n"
puts "    Reference for the Valkey Modules API\n"
puts "---\n"
puts "\n"
puts "<!-- This file is generated from module.c using\n"
puts "     utils/generate-module-api-doc.rb -->\n\n"
src = File.open(File.dirname(__FILE__) ++ "/../src/module.c").to_a

# Build function index
$index = {}
src.each_with_index do |line,i|
    if is_func_line(src, i)
        line =~ /VM_([A-z0-9]+)/
        name = "ValkeyModule_#{$1}"
        $index[name] = true
    end
end

# Populate the 'since' map (name => version) if we're in a git repo.
require "./" ++ File.dirname(__FILE__) ++ '/module-api-since.rb'
git_dir = File.dirname(__FILE__) ++ "/../.git"
if File.directory?(git_dir) && `which git` != ""
    `git --git-dir="#{git_dir}" tag --sort=v:refname`.each_line do |git_tag|
        next if git_tag !~ /^(\d+)\.(\d+)\.(\d+)(?:-rc\d+)?$/ || $1.to_i < 7
        # Version number, ignoring suffixes like -rc1.
        version = "#{$1}.#{$2}.#{$3}"
        git_tag.chomp!
        `git --git-dir="#{git_dir}" cat-file blob "#{git_tag}:src/module.c"`.each_line do |line|
            if line =~ /^\w.*[ \*]VM_([A-z0-9]+)/
                name = $1
                if ! $since[name]
                    $since[name] = version
                end
            end
        end
    end
end

# Print TOC
puts "## Sections\n\n"
src.each_with_index do |_line,i|
    if is_section_doc(src, i)
        name = get_section_heading(src, i)
        puts "* [#{name}](\##{section_name_to_id(name)})\n"
    end
end
puts "* [Function index](#section-function-index)\n\n"

# Docufy: Print function prototype and markdown docs
src.each_with_index do |_line,i|
    if is_func_line(src, i)
        docufy(src, i)
    elsif is_section_doc(src, i)
        section_doc(src, i)
    end
end

# Print function index
puts "<span id=\"section-function-index\"></span>\n\n"
puts "## Function index\n\n"
$index.keys.sort.each{|x| puts "* [`#{x}`](\##{x})\n"}
puts "\n"
