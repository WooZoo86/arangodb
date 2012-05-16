////////////////////////////////////////////////////////////////////////////////
/// @brief tests for query language, tenary operator
///
/// @file
///
/// DISCLAIMER
///
/// Copyright 2010-2012 triagens GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is triAGENS GmbH, Cologne, Germany
///
/// @author Jan Steemann
/// @author Copyright 2012, triAGENS GmbH, Cologne, Germany
////////////////////////////////////////////////////////////////////////////////

var jsunity = require("jsunity");

////////////////////////////////////////////////////////////////////////////////
/// @brief test suite
////////////////////////////////////////////////////////////////////////////////

function ahuacatlTernaryTestSuite () {

  ////////////////////////////////////////////////////////////////////////////////
/// @brief execute a given query
////////////////////////////////////////////////////////////////////////////////

  function executeQuery (query) {
    var cursor = AHUACATL_RUN(query, undefined);
    if (cursor instanceof ArangoError) {
      print(query, cursor.errorMessage);
    }
    assertFalse(cursor instanceof ArangoError);
    return cursor;
  }

////////////////////////////////////////////////////////////////////////////////
/// @brief execute a given query and return the results as an array
////////////////////////////////////////////////////////////////////////////////

  function getQueryResults (query) {
    var result = executeQuery(query).getRows();
    var results = [ ];

    for (var i in result) {
      if (!result.hasOwnProperty(i)) {
        continue;
      }

      results.push(result[i]);
    }

    return results;
  }


  return {

////////////////////////////////////////////////////////////////////////////////
/// @brief set up
////////////////////////////////////////////////////////////////////////////////

    setUp : function () {
    },

////////////////////////////////////////////////////////////////////////////////
/// @brief tear down
////////////////////////////////////////////////////////////////////////////////

    tearDown : function () {
    },

////////////////////////////////////////////////////////////////////////////////
/// @brief test ternary operator
////////////////////////////////////////////////////////////////////////////////
    
    testTernarySimple : function () {
      var expected = [ 2 ];
      var actual = getQueryResults("RETURN 1 > 0 ? 2 : -1");
      assertEqual(expected, actual);
    },

////////////////////////////////////////////////////////////////////////////////
/// @brief test ternary operator precedence
////////////////////////////////////////////////////////////////////////////////
    
    testTernaryNested1 : function () {
      var expected = [ -1 ];

      var actual = getQueryResults("RETURN 15 > 15 ? 1 : -1");
      assertEqual(expected, actual);
    },

////////////////////////////////////////////////////////////////////////////////
/// @brief test ternary operator precedence
////////////////////////////////////////////////////////////////////////////////
    
    testTernaryNested2 : function () {
      var expected = [ -1 ];

      var actual = getQueryResults("RETURN 10 + 5 > 15 ? 1 : -1");
      assertEqual(expected, actual);
    },

////////////////////////////////////////////////////////////////////////////////
/// @brief test ternary operator precedence
////////////////////////////////////////////////////////////////////////////////
    
    testTernaryNested3 : function () {
      var expected = [ 1 ];

      var actual = getQueryResults("RETURN true ? true ? true ? 1 : -1 : -2 : 3");
      assertEqual(expected, actual);
    },

////////////////////////////////////////////////////////////////////////////////
/// @brief test ternary operator precedence
////////////////////////////////////////////////////////////////////////////////
    
    testTernaryNested4 : function () {
      var expected = [ 3 ];

      var actual = getQueryResults("RETURN false ? true ? true ? 1 : -1 : -2 : 3");
      assertEqual(expected, actual);
    },

////////////////////////////////////////////////////////////////////////////////
/// @brief test ternary operator precedence
////////////////////////////////////////////////////////////////////////////////
    
    testTernaryNested5 : function () {
      var expected = [ -2 ];

      var actual = getQueryResults("RETURN true ? false ? true ? 1 : -1 : -2 : 3");
      assertEqual(expected, actual);
    },

////////////////////////////////////////////////////////////////////////////////
/// @brief test ternary operator precedence
////////////////////////////////////////////////////////////////////////////////
    
    testTernaryNested6 : function () {
      var expected = [ -1 ];

      var actual = getQueryResults("RETURN true ? true ? false ? 1 : -1 : -2 : 3");
      assertEqual(expected, actual);
    },

  };
}

////////////////////////////////////////////////////////////////////////////////
/// @brief executes the test suite
////////////////////////////////////////////////////////////////////////////////

jsunity.run(ahuacatlTernaryTestSuite);

return jsunity.done();

// Local Variables:
// mode: outline-minor
// outline-regexp: "^\\(/// @brief\\|/// @addtogroup\\|// --SECTION--\\|/// @page\\|/// @}\\)"
// End:
