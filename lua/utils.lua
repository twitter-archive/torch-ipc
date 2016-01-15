
-- Walk a table in a deterministic order
local function walkTable(t, f)
   local kk = { }
   for k,_ in pairs(t) do
      table.insert(kk, k)
   end
   table.sort(kk)
   for _,k in ipairs(kk) do
      local tk = t[k]
      if type(tk) == 'table' then
         walkTable(tk, f)
      else
         local tk1 = f(tk)
         if tk1 ~= nil then
            t[k] = tk1
         end
      end
   end
end

return {
   walkTable = walkTable
}
