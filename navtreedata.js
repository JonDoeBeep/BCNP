/*
 @licstart  The following is the entire license notice for the JavaScript code in this file.

 The MIT License (MIT)

 Copyright (C) 1997-2020 by Dimitri van Heesch

 Permission is hereby granted, free of charge, to any person obtaining a copy of this software
 and associated documentation files (the "Software"), to deal in the Software without restriction,
 including without limitation the rights to use, copy, modify, merge, publish, distribute,
 sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
 furnished to do so, subject to the following conditions:

 The above copyright notice and this permission notice shall be included in all copies or
 substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING
 BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
 DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

 @licend  The above is the entire license notice for the JavaScript code in this file
*/
var NAVTREE =
[
  [ "BCNP", "index.html", [
    [ "BCNP Core", "index.html", "index" ],
    [ "BCNP Schema & Code Generation", "md_schema.html", [
      [ "", "md_schema.html#autotoc_md1", null ],
      [ "Overview", "md_schema.html#autotoc_md2", null ],
      [ "Files", "md_schema.html#autotoc_md4", null ],
      [ "Quick Start", "md_schema.html#autotoc_md6", [
        [ "Define Messages", "md_schema.html#autotoc_md7", null ],
        [ "Generate Code", "md_schema.html#autotoc_md8", null ],
        [ "Use Generated Code", "md_schema.html#autotoc_md9", null ]
      ] ],
      [ "Adding a New Message Type", "md_schema.html#autotoc_md11", [
        [ "Step 1: Edit messages.json", "md_schema.html#autotoc_md12", null ],
        [ "Step 2: Run Codegen", "md_schema.html#autotoc_md13", null ],
        [ "Step 3: Rebuild & Update", "md_schema.html#autotoc_md14", null ]
      ] ],
      [ "Supported Field Types", "md_schema.html#autotoc_md16", [
        [ "Float Encoding", "md_schema.html#autotoc_md17", null ]
      ] ],
      [ "Message Definition Format", "md_schema.html#autotoc_md19", null ],
      [ "Schema Hash", "md_schema.html#autotoc_md21", null ],
      [ "Generated Output", "md_schema.html#autotoc_md23", [
        [ "C++ Header (message_types.h)", "md_schema.html#autotoc_md24", null ],
        [ "Python Module (bcnp_messages.py)", "md_schema.html#autotoc_md25", null ]
      ] ]
    ] ],
    [ "BCNP: Batched Command Network Protocol", "md__2github_2workspace_2protocol.html", [
      [ "", "md__2github_2workspace_2protocol.html#autotoc_md50", null ],
      [ "Table of Contents", "md__2github_2workspace_2protocol.html#autotoc_md51", null ],
      [ "Overview", "md__2github_2workspace_2protocol.html#autotoc_md53", null ],
      [ "Version History", "md__2github_2workspace_2protocol.html#autotoc_md55", null ],
      [ "Key Concepts", "md__2github_2workspace_2protocol.html#autotoc_md57", [
        [ "Registration-Based Serialization", "md__2github_2workspace_2protocol.html#autotoc_md58", null ],
        [ "Schema-Driven Development", "md__2github_2workspace_2protocol.html#autotoc_md59", null ],
        [ "Handshake Requirement", "md__2github_2workspace_2protocol.html#autotoc_md60", null ]
      ] ],
      [ "Wire Format", "md__2github_2workspace_2protocol.html#autotoc_md62", [
        [ "Schema Hash", "md__2github_2workspace_2protocol.html#autotoc_md63", null ],
        [ "Handshake Packet (8 bytes)", "md__2github_2workspace_2protocol.html#autotoc_md64", null ],
        [ "Data Packet", "md__2github_2workspace_2protocol.html#autotoc_md65", null ],
        [ "Header Fields", "md__2github_2workspace_2protocol.html#autotoc_md66", null ],
        [ "Homogeneous Packets", "md__2github_2workspace_2protocol.html#autotoc_md67", null ]
      ] ],
      [ "Message Types", "md__2github_2workspace_2protocol.html#autotoc_md69", [
        [ "Default: DriveCmd (ID: 1)", "md__2github_2workspace_2protocol.html#autotoc_md70", null ],
        [ "Fixed-Point Float Encoding", "md__2github_2workspace_2protocol.html#autotoc_md71", null ],
        [ "Defining Custom Message Types", "md__2github_2workspace_2protocol.html#autotoc_md72", null ],
        [ "Supported Field Types", "md__2github_2workspace_2protocol.html#autotoc_md73", null ]
      ] ],
      [ "Transport Layer", "md__2github_2workspace_2protocol.html#autotoc_md75", [
        [ "Handshake Protocol", "md__2github_2workspace_2protocol.html#autotoc_md76", null ],
        [ "Transport Guidelines", "md__2github_2workspace_2protocol.html#autotoc_md77", null ],
        [ "Sending Packets", "md__2github_2workspace_2protocol.html#autotoc_md78", null ]
      ] ],
      [ "Robot Behavior", "md__2github_2workspace_2protocol.html#autotoc_md80", [
        [ "Command Execution Model", "md__2github_2workspace_2protocol.html#autotoc_md81", null ],
        [ "Safety Features", "md__2github_2workspace_2protocol.html#autotoc_md82", null ],
        [ "SmartDashboard Keys", "md__2github_2workspace_2protocol.html#autotoc_md83", null ]
      ] ],
      [ "Code Generation", "md__2github_2workspace_2protocol.html#autotoc_md85", [
        [ "Running Codegen", "md__2github_2workspace_2protocol.html#autotoc_md86", null ],
        [ "Generated Files", "md__2github_2workspace_2protocol.html#autotoc_md87", null ],
        [ "After Schema Changes", "md__2github_2workspace_2protocol.html#autotoc_md88", null ]
      ] ],
      [ "Parser Diagnostics", "md__2github_2workspace_2protocol.html#autotoc_md90", null ]
    ] ],
    [ "Deprecated List", "deprecated.html", null ],
    [ "Topics", "topics.html", "topics" ],
    [ "Namespaces", "namespaces.html", [
      [ "Namespace List", "namespaces.html", "namespaces_dup" ],
      [ "Namespace Members", "namespacemembers.html", [
        [ "All", "namespacemembers.html", null ],
        [ "Functions", "namespacemembers_func.html", null ],
        [ "Variables", "namespacemembers_vars.html", null ],
        [ "Typedefs", "namespacemembers_type.html", null ],
        [ "Enumerations", "namespacemembers_enum.html", null ]
      ] ]
    ] ],
    [ "Classes", "annotated.html", [
      [ "Class List", "annotated.html", "annotated_dup" ],
      [ "Class Index", "classes.html", null ],
      [ "Class Hierarchy", "hierarchy.html", "hierarchy" ],
      [ "Class Members", "functions.html", [
        [ "All", "functions.html", null ],
        [ "Functions", "functions_func.html", null ],
        [ "Variables", "functions_vars.html", null ],
        [ "Typedefs", "functions_type.html", null ]
      ] ]
    ] ],
    [ "Files", "files.html", [
      [ "File List", "files.html", "files_dup" ]
    ] ]
  ] ]
];

var NAVTREEINDEX =
[
"adapter_8h.html",
"classbcnp_1_1StreamParser.html#aded412907bb5773c63587a3d58c3146a",
"namespacebcnp.html#ab9d83a77b2a1ac85b1c902bd414a1771a1b548e333201c69815e9ed82ef6525e3"
];

var SYNCONMSG = 'click to disable panel synchronisation';
var SYNCOFFMSG = 'click to enable panel synchronisation';