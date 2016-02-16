local paths = require 'paths'

local root = paths.dirname(paths.thisfile())
require(paths.concat(root, 'test_workqueue'))
require(paths.concat(root, 'test_cliser'))
require(paths.concat(root, 'test_map'))
require(paths.concat(root, 'test_Tree'))
